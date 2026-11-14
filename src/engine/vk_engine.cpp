// The Vulkan engine: imports captured frames (DMA-BUF or memory), composites
// them on the GPU with compute shaders, and reads the result back for the
// preview and the encoders. Every public operation that needs the device is
// recorded into one command buffer and flushed before a readback is mapped;
// the frame loop's readbacks are instead submitted without waiting and taken
// a tick later (requestOutput), and anything else waits for them first.
// Usually it works on the Vulkan device FFmpeg makes for its GPU, sharing a
// queue with FFmpeg's encoders under FFmpeg's queue lock, so an encoder there
// takes its pictures without them leaving the GPU (writeVulkanFrames).

#include <vulkan/vulkan.h>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <utility>
#include <unistd.h>

#include <QByteArray>
#include <QDebug>
#include <QFile>

#include "engine.h"
#include "matting.h"

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

namespace engine {
namespace {

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
}
constexpr uint32_t kXR24 = fourcc('X', 'R', '2', '4');
constexpr uint32_t kAR24 = fourcc('A', 'R', '2', '4');
constexpr uint32_t kXB24 = fourcc('X', 'B', '2', '4');
constexpr uint32_t kAB24 = fourcc('A', 'B', '2', '4');
constexpr uint32_t kXR30 = fourcc('X', 'R', '3', '0');
constexpr uint32_t kAB30 = fourcc('A', 'B', '3', '0');

struct FormatInfo {
    VkFormat vk = VK_FORMAT_UNDEFINED;
    bool alpha = false;
};

FormatInfo formatForFourcc(uint32_t f) {
    switch (f) {
    case kXR24: return {VK_FORMAT_B8G8R8A8_UNORM, false};
    case kAR24: return {VK_FORMAT_B8G8R8A8_UNORM, true};
    case kXB24: return {VK_FORMAT_R8G8B8A8_UNORM, false};
    case kAB24: return {VK_FORMAT_R8G8B8A8_UNORM, true};
    case kXR30: return {VK_FORMAT_A2R10G10B10_UNORM_PACK32, false};
    case kAB30: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, false};
    default: return {};
    }
}

FormatInfo formatForPixel(PixelFormat f) {
    switch (f) {
    case PixelFormat::BGRx: return {VK_FORMAT_B8G8R8A8_UNORM, false};
    case PixelFormat::BGRA: return {VK_FORMAT_B8G8R8A8_UNORM, true};
    case PixelFormat::RGBx: return {VK_FORMAT_R8G8B8A8_UNORM, false};
    case PixelFormat::RGBA: return {VK_FORMAT_R8G8B8A8_UNORM, true};
    }
    return {};
}

inline uint32_t align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
inline uint32_t groups(uint32_t n, uint32_t local) { return (n + local - 1) / local; }

QByteArray loadSpirv(const char *name) {
    QFile f(QStringLiteral(":/shaders/") + QLatin1String(name));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    int width = 0;
    int height = 0;
    bool external = false;
    bool initialised = false;  // has had its first layout transition
    std::shared_ptr<const void> owner; // keep cached DMA-BUF identities alive
    bool clearable = false;    // made with TRANSFER_DST, so a composite can clear it
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
};

struct Source {
    Image work;                                    // RGBA16F, linear
    Image previous, fieldFrame;
    DeinterlaceMode deinterlace = DeinterlaceMode::Off;
    bool previousValid = false, resetNext = false, fieldReady = false, pendingProcess = false;
    int field = 0;
    bool firstField = true;
    Image upload;                                  // unorm copy of a memory frame
    std::unordered_map<const void *, Image> imports;  // DMA-BUF imports by buffer token
    bool hasFrame = false;
    // Noise reduction: two cleaned frames used in turn, the other being the
    // history the next frame is averaged with.
    float denoise = 0.f;
    Image clean[2];
    int cleanIndex = 0;
    bool historyValid = false;
    const Image &processed() const { return deinterlace != DeinterlaceMode::Off && fieldReady ? fieldFrame : work; }
    const Image &base() const { return denoise > 0.f && historyValid ? clean[cleanIndex] : processed(); }
    // Video filters, as in cpu_engine.cpp: one picture per step, or a delay's
    // held pictures, oldest first, the shown one at the front; `aux` is the
    // step's LUT or mask on the device, uploaded from `auxOf`.
    struct Step {
        Image out, aux, mid;  // mid: a two-pass step's first pass; aux: also RemoveBackground's matte
        const void *auxOf = nullptr;
        matting::Plan plan;   // RemoveBackground's network, its working space,
        Buffer arena;         // and the part of a run to do next (0: none under way)
        size_t part = 0;
        // Its matte's pictures after the network (see recordGuided), whether
        // the last frame's are there to steady it with, and which they are.
        Image guide[6], back[2];
        bool history = false;
        int flip = 0;
        std::deque<std::pair<Image, double>> held;
        std::vector<Image> spare;
        double origin = 0;
        bool started = false;
        const Image &output() const { return held.empty() ? out : held.front().first; }
    };
    std::vector<VideoFilter> filters;
    std::vector<Step> steps;
    bool filtered = false;
    uint64_t generation = 0, filteredGeneration = ~0ull;
    const Image &shown() const { return filtered && !steps.empty() ? steps.back().output() : base(); }
    bool hide = false;  // setSourceHidden
    bool hidden() const { return hide || (!filtered && chainFailsClosed(filters)); }
    FilterFailure failure = FilterFailure::None;
    double retryAt = 0;  // a chain that failed is not tried again before this
};

struct Pipeline {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

// Push constants, one struct per shader, laid out as its `Push` block. Each
// pipeline's push range is the struct's size, so the two cannot drift apart.
struct TransitionPush {
    int32_t w, h, type; float progress;
    int32_t side, mask, invert; float softness;
    float colourPoint, pad[3];
    float colour[4];
    float matte[4];
};
struct ConvertPush {
    int32_t w, h, flags;
};
struct CompositePush {
    float x, y, w, h;
    float u0, v0, u1, v1;
    float cosR, sinR;
    int32_t ox, oy;
    int32_t cw, ch;
    float opacity;
    int32_t filter;
    float footprintX, footprintY;
    int32_t blend, blendSpace, pad1, pad2;
};
struct YuvOutPush {
    int32_t w, h, yStride, cStride, uOffset, vOffset;
    int32_t canvasW, canvasH;
    OutputParams q;
};
struct PreviewPush {
    int32_t w, h;
};
struct YuvInPush {
    int32_t w, h, layout, full;
    float kr, kb;
    int32_t interlaced;
    int32_t alphaRow, premultiplied, linearAlpha;
    int32_t depth, shift, kind, wide;
    float scale, gamma1;
    float gamut[9];
};
struct DeinterlacePush { int32_t w, h, mode, field, first, history; };
struct DenoisePush {
    int32_t w, h;
    float sigma, threshold, keepMin;
    int32_t fresh;
};
struct FilterPush {
    int32_t w, h, ow, oh;
    int32_t kind, mode, ix, iy;
    float p[24];
};
static_assert(sizeof(CompositePush) == 88 && sizeof(DenoisePush) == 24 && sizeof(FilterPush) == 128, "push blocks match the shaders");
static_assert(sizeof(YuvOutPush) == 128 && sizeof(YuvInPush) == 100, "push blocks match the shaders, within the 128 bytes every device has");

class VkEngine final : public Engine {
public:
    ~VkEngine() override { shutdown(); }

    bool init(int deviceIndex, std::string *failure, AVBufferRef *shared = nullptr);

    bool isGpu() const override { return true; }
    std::string deviceName() const override { return m_deviceName; }
    std::string renderNode() const override { return m_renderNode; }
    bool lost() const override { return m_lost; }
    FilterFailure sourceFilterFailure(int id) const override {
        const auto it = m_sources.find(id);
        return it == m_sources.end() ? FilterFailure::None : it->second.failure;
    }
    void setSourceHidden(int id, bool hidden) override {
        const auto it = m_sources.find(id);
        if (it != m_sources.end()) it->second.hide = hidden;
    }
    bool sourceHidden(int id) const override {
        const auto it = m_sources.find(id);
        return it != m_sources.end() && it->second.hide;
    }

    int createSource() override {
        const int id = m_nextId++;
        m_sources.emplace(id, Source{});
        return id;
    }
    void destroySource(int id) override {
        auto it = m_sources.find(id);
        if (it == m_sources.end()) return;
        flush();
        m_filterMs.erase(id);
        m_wrapMs.erase(id);
        destroyImage(it->second.work);
        destroyImage(it->second.previous);
        destroyImage(it->second.fieldFrame);
        destroyImage(it->second.upload);
        destroyImage(it->second.clean[0]);
        destroyImage(it->second.clean[1]);
        for (auto &kv : it->second.imports) destroyImage(kv.second);
        releaseSteps(it->second);
        m_sources.erase(it);
    }
    void setSourceDenoise(int id, float strength) override {
        auto it = m_sources.find(id);
        if (it == m_sources.end()) return;
        if ((strength > 0.f) != (it->second.denoise > 0.f)) it->second.historyValid = false;
        it->second.denoise = strength;
        it->second.pendingProcess = true;
    }

    void setSourceDeinterlace(int id, DeinterlaceMode mode) override;
    void renderSourceField(int id, int field, bool firstInFrame) override;
    void setSourceFilters(int id, const std::vector<VideoFilter> &filters) override;
    void renderSourceFilters(int id, double seconds) override;
    void resetSourceHistory(int id) override;
    bool updateSource(int id, const CpuFrame &f) override;
    bool updateSource(int id, const DmaBufFrame &f) override;
    bool updateSource(int id, const YuvInput &f) override;
    std::vector<uint64_t> importModifiers(uint32_t fourcc) const override;

    void setCanvasSize(int w, int h) override {
        if (w <= 0 || h <= 0 || (m_canvas.width == w && m_canvas.height == h)) return;
        flush();
        destroyImage(m_canvas);
        // No canvas, no engine: taken as lost, and made again.
        if (!createImage(m_canvas, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            fail(VK_ERROR_DEVICE_LOST);
    }
    int canvasWidth() const override { return m_canvas.width; }
    int canvasHeight() const override { return m_canvas.height; }

    void composite(const std::vector<Layer> &layers, const Color &bg) override;
    void compositeInto(int target, int width, int height, const std::vector<Layer> &layers, const Color &bg) override;
    void transitionInto(int target, int width, int height, int from, int to, const TransitionStyle &style, float progress) override;
    bool readPreview(int w, int h, Bgra8Frame &out) override;
    bool readSourcePreview(int source, int w, int h, Bgra8Frame &out) override;
    bool readSourceInput(int source, int w, int h, Bgra8Frame &out) override;
    bool readOutput(const OutputColour &colour, YuvFrame &out, int width, int height) override;
    bool readSourceOutput(int source, const OutputColour &colour, YuvFrame &out, int width, int height) override;
    void setHdrLevels(const HdrLevels &levels) override {
        if (levels == m_levels) return;
        m_levels = levels;
        for (auto &kv : m_sources) kv.second.filtered = false;  // tone maps read the white level
    }
    HdrLevels hdrLevels() const override { return m_levels; }
    void flush() override;

    int requestOutput(const OutputColour &colour, int width, int height) override;
    int requestPreview(int width, int height) override;
    void submit() override;
    void collect() override { settle(); }
    bool takeOutput(int request, YuvFrame &out) override;
    bool takePreview(int request, Bgra8Frame &out) override;
    const void *sharedDevice() const override { return m_ffDevice ? m_ffDevice->data : nullptr; }
    bool writeVulkanFrames(const OutputColour &colour, int width, int height, const std::vector<AVFrame *> &frames) override;

private:
    // On FFmpeg's device: its handles, a compute queue of its, and its queue
    // lock around our submissions (its encoders submit to the same queues).
    bool adopt(AVBufferRef *shared, std::string *failure);
    void lockQueue();
    void unlockQueue();
    // Timeline semaphores the next submission waits for and signals (frames
    // handed to FFmpeg's encoders).
    std::vector<VkSemaphore> m_waitSems, m_signalSems;
    std::vector<uint64_t> m_waitValues, m_signalValues;
    std::vector<VkPipelineStageFlags> m_waitStages;
    bool submitRecorded();  // ends m_cmd and submits it with m_fence and the semaphores
    AVBufferRef *m_ffDevice = nullptr;
    uint32_t m_queueIndex = 0;
    // Where a readback's planes sit in its host buffer, and its size.
    struct Planes {
        uint32_t offsets[3] = {};
        uint32_t strides[3] = {};
        VkDeviceSize size = 0;
    };
    // A readback asked for and not yet taken, with buffers of its own.
    struct Readback {
        int id = 0;               // 0: free; -1: feeding a frame handed to an encoder, free once that work is done
        bool preview = false;
        YuvFrame shape;           // the picture's size and format, no samples
        Planes planes;
        Buffer device, host;
    };

    bool previewOf(const Image &picture, int w, int h, Bgra8Frame &out);
    bool outputOf(const Image &picture, const OutputColour &colour, YuvFrame &out, int width, int height);
    // Records the conversion into `device` and its copy into `host`, sized
    // by `shape`; the samples are there after the work is finished.
    bool recordOutput(const Image &picture, const OutputColour &colour, const YuvFrame &shape, Buffer &device,
                      Buffer &host, Planes *planes, bool toHost = true);
    static void copyOutput(const Buffer &host, const Planes &planes, YuvFrame &out);
    bool recordPreview(const Image &picture, int w, int h, Buffer &device, Buffer &host);
    Readback &freeReadback();
    // Waits for what submit() sent.
    void settle();
    void waitForFence();
    // Setup
    bool createInstance(std::string *failure);
    bool pickDevice(int index, std::string *failure);
    bool createDevice(std::string *failure);
    bool createPipelines(std::string *failure);
    bool createPipeline(Pipeline &p, const char *spv, VkDescriptorType binding1, uint32_t pushSize,
                        std::string *failure, bool secondSampler = false);
    bool createPipeline(Pipeline &p, const char *spv, const std::vector<VkDescriptorType> &bindings, uint32_t pushSize,
                        std::string *failure);
    void shutdown();
    bool lastWait();
    void abandon();

    // Resources
    bool createImage(Image &img, int w, int h, VkFormat fmt, VkImageUsageFlags usage);
    bool importDmaBuf(Image &img, const DmaBufFrame &f, const FormatInfo &fi);
    void destroyImage(Image &img);
    bool createBuffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void destroyBuffer(Buffer &b);
    bool ensureBuffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
        if (b.size >= size && b.buffer) return true;
        destroyBuffer(b);
        return createBuffer(b, size, usage, hostVisible);
    }
    int32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred);
    bool stagingRoom(VkDeviceSize bytes, VkDeviceSize *offset);

    // Recording
    bool begin();
    void imageBarrier(Image &img, VkImageLayout from, VkImageLayout to, bool acquireForeign = false);
    void memoryBarrier();
    VkDescriptorSet descriptorSet(const Pipeline &p, VkImageView sampled, VkImageView storage,
                                  VkBuffer buffer, VkDeviceSize bufferSize, VkImageView sampled2 = VK_NULL_HANDLE);
    VkDescriptorSet allocateSet(const Pipeline &p);
    // Cleans the source's newly converted frame when it asks for noise reduction.
    void beginFrame(Source &s, int w, int h);
    void finishFrame(Source &s);
    bool recordDeinterlace(Source &s, int field, bool first);
    void recordDenoise(Source &s);
    void compositeTo(Image &into, int skip, const std::vector<Layer> &layers, const Color &bg);
    void clearTo(Image &into, const Color &bg);
    void drawLayer(Image &into, const Image &shown, const Layer &l);
    bool filterImage(Image &img, int w, int h, VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT);
    bool uploadAux(Image &img, VkFormat format, int w, int h, const void *data, size_t bytes);
    // What a source shows, or one clear picture when it must not be seen.
    const Image &shownOf(const Source &s) const { return s.hidden() ? m_nothing : s.shown(); }
    bool createNothing(std::string *failure);
    bool recordMatte(const Image &in, Source::Step &st, const FilterParams &q, int parts, bool fresh, FilterFailure &why);
    bool wrapLayer(Image &into, const Image &shown, const Layer &l, const VideoFilter &f);
    bool recordGuided(const Image &in, Source::Step &st, const VideoFilter &f, const FilterParams &q);
    void recordFilter(const Image &in, Image &out, const Image *aux, const FilterParams &q);
    void releaseSteps(Source &s);
    void recordConvert(Source &s, const Image &from, bool forceOpaque, bool premultiplied = false, bool limitedRange = false, bool linearAlpha = true, bool yInvert = false, BufferTransform transform = BufferTransform::Normal);
    void fail(VkResult r);

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_family = 0;
    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    // GPU timestamps around each source's filters: written while recording,
    // read once the fence says the work is done (never waited for).
    static constexpr uint32_t kTimestamps = 128;
    VkQueryPool m_timestamps = VK_NULL_HANDLE;
    double m_tickNs = 0;            // ns per timestamp unit; 0: this queue cannot time
    uint32_t m_timestampsUsed = 0;
    uint64_t m_bufferSerial = 0;    // counts command buffers begun
    // A source's chain, or its light wrap as it is drawn (which counts as its filters too).
    struct Timing { int source; uint32_t first; bool wrap = false; };
    std::vector<Timing> m_timings, m_timingsInFlight;
    std::unordered_map<int, double> m_filterMs, m_wrapMs;
    void harvestTimings();
public:
    double filterMs(int id) const override {
        const auto it = m_filterMs.find(id), wrap = m_wrapMs.find(id);
        if (it == m_filterMs.end()) return -1;
        return it->second + (wrap == m_wrapMs.end() ? 0.0 : wrap->second);
    }
private:
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    // More, made when a frame needs more sets than one pool holds (many
    // sources and filters); reset with the first at each begin().
    std::vector<VkDescriptorPool> m_extraPools;
    size_t m_extraUsed = 0;
    VkDescriptorPool m_currentPool = VK_NULL_HANDLE;  // the one sets come from now
    VkDescriptorPool makeDescriptorPool();
    void hostReadBarrier();
    std::vector<Image> m_doomed;  // given up while lost; destroyed in the destructor
    std::vector<Buffer> m_doomedBuffers;  // the same
    bool m_tearingDown = false;
    VkSampler m_linear = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties m_memProps{};
    PFN_vkGetMemoryFdPropertiesKHR m_getMemoryFdProperties = nullptr;
    bool m_hasDmaBuf = false;
    bool m_hasForeignQueue = false;
    bool m_recording = false;
    bool m_lost = false;
    // Work the queue took whose fence has not been seen to finish, and whether
    // the last wait for it ran out with no word from the driver (see lastWait).
    bool m_inFlight = false, m_silent = false;
    std::string m_deviceName;
    std::string m_renderNode;

    Pipeline m_transition, m_convert, m_composite, m_yuv, m_preview, m_yuvIn, m_denoise, m_deinterlace, m_filter, m_filter32, m_nn;
    // RemoveBackground's network: its weights, uploaded the first time.
    Buffer m_nnWeights;
    // Light wrap's pictures, for each size it draws into.
    // Kept and let go by composites counted (see kWrapSetsKept in frame.h).
    struct Wrap {
        int width = 0, height = 0, scale = 4;
        Image images[4];
        uint64_t used = 0;  // the composite that last drew with them
    };
    std::vector<Wrap> m_wraps;
    uint64_t m_composites = 0;
    VkDeviceSize m_maxStorage = 0;  // the largest storage buffer a shader may see
    Image m_nothing;                // see shownOf
    uint32_t m_maxImage = 0;        // the widest and tallest picture the device makes

    std::unordered_map<int, Source> m_sources;
    HdrLevels m_levels;
    int m_nextId = 1;
    Image m_canvas;
    Buffer m_staging;        // host -> device uploads
    VkDeviceSize m_stagingUsed = 0;  // of it, by the uploads recorded in this command buffer
    Buffer m_auxStaging;     // LUT and mask uploads
    Buffer m_yuvDevice, m_yuvHost;
    std::vector<Readback> m_readbacks;
    bool m_submitted = false;     // submit() sent work nobody has waited for
    Buffer m_previewDevice, m_previewHost;
    std::vector<VkDescriptorSet> m_usedSets;
};

// ---------------------------------------------------------------- setup

static std::string renderNodeOf(VkPhysicalDevice device);

bool VkEngine::init(int deviceIndex, std::string *failure, AVBufferRef *shared) {
    std::lock_guard<std::recursive_mutex> lock(loaderMutex());
    if (shared) return adopt(shared, failure) && createDevice(failure) && createPipelines(failure) && createNothing(failure);
    return createInstance(failure) && pickDevice(deviceIndex, failure) && createDevice(failure) &&
           createPipelines(failure) && createNothing(failure);
}

bool VkEngine::adopt(AVBufferRef *shared, std::string *failure) {
    auto *dc = reinterpret_cast<AVHWDeviceContext *>(shared->data);
    if (dc->type != AV_HWDEVICE_TYPE_VULKAN) {
        if (failure) *failure = "not a Vulkan device";
        return false;
    }
    const auto *vk = static_cast<const AVVulkanDeviceContext *>(dc->hwctx);
    m_ffDevice = av_buffer_ref(shared);
    m_instance = vk->inst;
    m_physical = vk->phys_dev;
    m_device = vk->act_dev;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_physical, &props);
    m_deviceName = props.deviceName;
    m_renderNode = renderNodeOf(m_physical);
    vkGetPhysicalDeviceMemoryProperties(m_physical, &m_memProps);
    return true;
}

void VkEngine::lockQueue() {
    if (!m_ffDevice) return;
    auto *dc = reinterpret_cast<AVHWDeviceContext *>(m_ffDevice->data);
    auto *vk = static_cast<AVVulkanDeviceContext *>(dc->hwctx);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (vk->lock_queue) vk->lock_queue(dc, m_family, m_queueIndex);
#pragma GCC diagnostic pop
}

void VkEngine::unlockQueue() {
    if (!m_ffDevice) return;
    auto *dc = reinterpret_cast<AVHWDeviceContext *>(m_ffDevice->data);
    auto *vk = static_cast<AVVulkanDeviceContext *>(dc->hwctx);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (vk->unlock_queue) vk->unlock_queue(dc, m_family, m_queueIndex);
#pragma GCC diagnostic pop
}

// The device's DRM render node, from VK_EXT_physical_device_drm when the
// driver offers it.
static std::string renderNodeOf(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, exts.data());
    bool has = false;
    for (const VkExtensionProperties &e : exts)
        if (std::strcmp(e.extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0) has = true;
    if (!has) return {};
    VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &drm;
    vkGetPhysicalDeviceProperties2(device, &props);
    if (!drm.hasRender) return {};
    return "/dev/dri/renderD" + std::to_string(drm.renderMinor);
}

std::vector<GpuDevice> enumerateGpus() {
    std::lock_guard<std::recursive_mutex> lock(loaderMutex());
    std::vector<GpuDevice> out;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "omabroadcast";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) return out;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &ids;
        vkGetPhysicalDeviceProperties2(devices[i], &props);
        const VkPhysicalDeviceProperties &p = props.properties;
        GpuDevice g;
        g.index = int(i);
        static const char hex[] = "0123456789abcdef";
        for (uint8_t b : ids.deviceUUID) { g.id += hex[b >> 4]; g.id += hex[b & 15]; }
        g.name = p.deviceName;
        g.vendorId = p.vendorID;
        g.renderNode = renderNodeOf(devices[i]);
        g.kind = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? GpuDevice::Kind::Discrete
               : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? GpuDevice::Kind::Integrated
               : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? GpuDevice::Kind::Virtual
                                                                        : GpuDevice::Kind::Other;
        g.hardware = p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_OTHER;
        g.usable = g.hardware && p.apiVersion >= VK_API_VERSION_1_2;
        out.push_back(g);
    }
    vkDestroyInstance(instance, nullptr);
    return out;
}

bool VkEngine::createInstance(std::string *failure) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "omabroadcast";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    const VkResult r = vkCreateInstance(&ci, nullptr, &m_instance);
    if (r != VK_SUCCESS) {
        if (failure) *failure = "no Vulkan instance (" + std::to_string(r) + ")";
        return false;
    }
    return true;
}

bool VkEngine::pickDevice(int index, std::string *failure) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) {
        if (failure) *failure = "no Vulkan device";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    auto score = [](VkPhysicalDevice d) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.apiVersion < VK_API_VERSION_1_2) return -1;
        switch (p.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 1;
        default: return 0;  // the CPU twin beats a software Vulkan device
        }
    };

    int best = -1;
    if (index >= static_cast<int>(count)) {
        if (failure) *failure = "no Vulkan device at index " + std::to_string(index);
        return false;
    }
    if (index >= 0) {
        best = index;
    } else {
        int bestScore = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const int s = score(devices[i]);
            if (s > bestScore) { bestScore = s; best = static_cast<int>(i); }
        }
    }
    if (best < 0) {
        if (failure) *failure = "no suitable Vulkan device";
        return false;
    }
    m_physical = devices[best];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_physical, &props);
    m_deviceName = props.deviceName;
    m_renderNode = renderNodeOf(m_physical);
    vkGetPhysicalDeviceMemoryProperties(m_physical, &m_memProps);
    return true;
}

bool VkEngine::createDevice(std::string *failure) {
    if (m_ffDevice) {
        const auto *vk = static_cast<const AVVulkanDeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(m_ffDevice->data)->hwctx);
        bool found = false;
        for (int i = 0; i < vk->nb_qf && !found; ++i) {
            if (!(vk->qf[i].flags & VK_QUEUE_COMPUTE_BIT)) continue;
            m_family = static_cast<uint32_t>(vk->qf[i].idx);
            m_queueIndex = static_cast<uint32_t>(std::max(0, vk->qf[i].num - 1));
            found = true;
        }
        if (!found) {
            if (failure) *failure = "no compute queue on FFmpeg's device";
            return false;
        }
        auto enabled = [&](const char *name) {
            for (int i = 0; i < vk->nb_enabled_dev_extensions; ++i)
                if (std::strcmp(vk->enabled_dev_extensions[i], name) == 0) return true;
            return false;
        };
        m_hasDmaBuf = enabled(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) && enabled(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
                      enabled(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
        m_hasForeignQueue = m_hasDmaBuf && enabled(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
        VkDeviceQueueInfo2 qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
        qi.flags = vk->queue_flags;
        qi.queueFamilyIndex = m_family;
        qi.queueIndex = m_queueIndex;
        vkGetDeviceQueue2(m_device, &qi, &m_queue);
    } else {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qCount, families.data());
        bool found = false;
        for (uint32_t i = 0; i < qCount; ++i) {
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                m_family = i;
                found = true;
                break;
            }
        }
        if (!found) {
            if (failure) *failure = "no compute queue";
            return false;
        }

        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &extCount, exts.data());
        auto has = [&](const char *name) {
            return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties &e) {
                return std::strcmp(e.extensionName, name) == 0;
            });
        };
        std::vector<const char *> enable;
        if (has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) && has(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
            has(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME)) {
            enable.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
            enable.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
            enable.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
            m_hasDmaBuf = true;
            if (has(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
                enable.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
                m_hasForeignQueue = true;
            }
        }

        const float priority = 1.f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = m_family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
        dci.ppEnabledExtensionNames = enable.data();
        VkResult r = vkCreateDevice(m_physical, &dci, nullptr, &m_device);
        if (r != VK_SUCCESS) {
            if (failure) *failure = "device creation failed (" + std::to_string(r) + ")";
            return false;
        }
        vkGetDeviceQueue(m_device, m_family, 0, &m_queue);
    }
    if (m_hasDmaBuf) {
        m_getMemoryFdProperties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
            vkGetDeviceProcAddr(m_device, "vkGetMemoryFdPropertiesKHR"));
        if (!m_getMemoryFdProperties) m_hasDmaBuf = false;
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_family;
    if (vkCreateCommandPool(m_device, &pci, nullptr, &m_cmdPool) != VK_SUCCESS) {
        if (failure) *failure = "command pool";
        return false;
    }
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = m_cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cai, &m_cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(m_device, &fci, nullptr, &m_fence);
    {
        // Timestamps, where the queue keeps them (filter costs; nothing else depends on them).
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(m_physical, &props);
        m_maxStorage = props.limits.maxStorageBufferRange;
        m_maxImage = props.limits.maxImageDimension2D;
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &n, families.data());
        if (m_family < n && families[m_family].timestampValidBits > 0 && props.limits.timestampPeriod > 0) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = kTimestamps;
            if (vkCreateQueryPool(m_device, &qi, nullptr, &m_timestamps) == VK_SUCCESS) m_tickNs = props.limits.timestampPeriod;
        }
    }

    m_descPool = m_currentPool = makeDescriptorPool();
    if (!m_descPool) {
        if (failure) *failure = "descriptor pool";
        return false;
    }

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.f;
    vkCreateSampler(m_device, &sci, nullptr, &m_linear);
    return true;
}

bool VkEngine::createPipeline(Pipeline &p, const char *spv, VkDescriptorType binding1, uint32_t pushSize,
                              std::string *failure, bool secondSampler) {
    std::vector<VkDescriptorType> bindings{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, binding1};
    if (secondSampler) bindings.push_back(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    return createPipeline(p, spv, bindings, pushSize, failure);
}

bool VkEngine::createPipeline(Pipeline &p, const char *spv, const std::vector<VkDescriptorType> &types, uint32_t pushSize,
                              std::string *failure) {
    const QByteArray code = loadSpirv(spv);
    if (code.isEmpty() || code.size() % 4 != 0) {
        if (failure) *failure = std::string("shader missing: ") + spv;
        return false;
    }
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = static_cast<size_t>(code.size());
    smci.pCode = reinterpret_cast<const uint32_t *>(code.constData());
    VkShaderModule module;
    if (vkCreateShaderModule(m_device, &smci, nullptr, &module) != VK_SUCCESS) {
        if (failure) *failure = std::string("shader rejected: ") + spv;
        return false;
    }

    std::vector<VkDescriptorSetLayoutBinding> bindings(types.size());
    for (size_t i = 0; i < types.size(); ++i) {
        bindings[i].binding = uint32_t(i);
        bindings[i].descriptorType = types[i];
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = uint32_t(bindings.size());
    dlci.pBindings = bindings.data();
    vkCreateDescriptorSetLayout(m_device, &dlci, nullptr, &p.layout);

    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushSize};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &p.layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    vkCreatePipelineLayout(m_device, &plci, nullptr, &p.pipelineLayout);

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName = "main";
    cpci.layout = p.pipelineLayout;
    const VkResult r = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &p.pipeline);
    vkDestroyShaderModule(m_device, module, nullptr);
    if (r != VK_SUCCESS) {
        if (failure) *failure = std::string("pipeline failed: ") + spv;
        return false;
    }
    return true;
}

bool VkEngine::createPipelines(std::string *failure) {
    return createPipeline(m_transition, "transition.spv", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizeof(TransitionPush), failure, true) &&
           createPipeline(m_convert, "convert.spv", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizeof(ConvertPush), failure) &&
           createPipeline(m_composite, "composite.spv", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizeof(CompositePush),
                          failure) &&
           createPipeline(m_yuv, "yuv.spv", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sizeof(YuvOutPush), failure) &&
           createPipeline(m_preview, "preview.spv", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sizeof(PreviewPush), failure) &&
           createPipeline(m_yuvIn, "yuv_in.spv", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizeof(YuvInPush), failure) &&
           createPipeline(m_deinterlace, "deinterlace.spv", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizeof(DeinterlacePush), failure, true) &&
           createPipeline(m_denoise, "denoise.spv", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizeof(DenoisePush), failure,
                          true) &&
           createPipeline(m_filter, "filter.spv", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizeof(FilterPush), failure, true) &&
           createPipeline(m_filter32, "filter32.spv", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizeof(FilterPush), failure, true) &&
           createPipeline(m_nn, "nn.spv",
                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                          sizeof(matting::Pass), failure);
}

// Whether everything sent to the queue has finished, so that what it used may
// be destroyed. All of it goes in one command buffer with one fence, so that
// fence is what says; and no wait here is without limit, for a GPU that has
// stopped answering would hold the frame thread (and with it the switch to the
// CPU, or the program's end) for as long as it stayed silent.
bool VkEngine::lastWait() {
    if (!m_inFlight) return true;
    if (m_silent) {
        // A minute's waiting ran out on this work already. One more look, for a
        // GPU that was only slow; still nothing, and it is left alone.
        const VkResult r = vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, 1'000'000'000ull);
        if (r == VK_TIMEOUT) return false;
        if (r == VK_SUCCESS) { m_inFlight = false; return true; }
    } else {
        // Sent and not yet collected, or the driver called the device lost
        // while waiting for it: the usual patience, once more.
        waitForFence();
        if (!m_inFlight) return true;
        if (m_silent) return false;
    }
    // The driver has said the device is lost, and a lost device answers these at
    // once. On FFmpeg's device its encoders may still be submitting: our own
    // queue only (theirs are theirs), under its lock.
    if (m_ffDevice) {
        lockQueue();
        vkQueueWaitIdle(m_queue);
        unlockQueue();
    } else {
        vkDeviceWaitIdle(m_device);
    }
    m_inFlight = false;
    return true;
}

// A GPU that never answered may still be working with everything made on it,
// and asking it to stop may never return: nothing is destroyed and nothing more
// is asked of it. What it held stays with it for the life of the program. On
// FFmpeg's device our hold on the device stays too, so that the device outlives
// what is left on it.
void VkEngine::abandon() {
    qWarning("%s never answered: what it was given is left with it.", m_deviceName.c_str());
    m_ffDevice = nullptr;
    m_device = VK_NULL_HANDLE;
    m_instance = VK_NULL_HANDLE;
}

void VkEngine::shutdown() {
    // The waiting first, outside the loader's turn-taking: other threads list
    // GPUs and make FFmpeg devices behind that lock.
    if (m_device && !lastWait()) {
        abandon();
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(loaderMutex());
    if (!m_device) {
        if (m_instance) vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
        return;
    }
    m_submitted = false;
    m_tearingDown = true;  // waited for: destroyed for real from here
    for (Image &img : m_doomed) destroyImage(img);
    m_doomed.clear();
    for (Buffer &b : m_doomedBuffers) destroyBuffer(b);
    m_doomedBuffers.clear();
    for (auto &kv : m_sources) {
        destroyImage(kv.second.work);
        destroyImage(kv.second.previous);
        destroyImage(kv.second.fieldFrame);
        destroyImage(kv.second.upload);
        destroyImage(kv.second.clean[0]);
        destroyImage(kv.second.clean[1]);
        for (auto &imp : kv.second.imports) destroyImage(imp.second);
        releaseSteps(kv.second);
    }
    m_sources.clear();
    destroyImage(m_nothing);
    destroyImage(m_canvas);
    destroyBuffer(m_staging);
    destroyBuffer(m_auxStaging);
    destroyBuffer(m_yuvDevice);
    destroyBuffer(m_yuvHost);
    destroyBuffer(m_previewDevice);
    destroyBuffer(m_previewHost);
    destroyBuffer(m_nnWeights);
    for (Wrap &w : m_wraps) for (Image &img : w.images) destroyImage(img);
    m_wraps.clear();
    for (Readback &r : m_readbacks) {
        destroyBuffer(r.device);
        destroyBuffer(r.host);
    }
    m_readbacks.clear();
    for (Pipeline *p : {&m_transition, &m_convert, &m_composite, &m_yuv, &m_preview, &m_yuvIn, &m_denoise, &m_deinterlace, &m_filter, &m_filter32, &m_nn}) {
        if (p->pipeline) vkDestroyPipeline(m_device, p->pipeline, nullptr);
        if (p->pipelineLayout) vkDestroyPipelineLayout(m_device, p->pipelineLayout, nullptr);
        if (p->layout) vkDestroyDescriptorSetLayout(m_device, p->layout, nullptr);
    }
    if (m_linear) vkDestroySampler(m_device, m_linear, nullptr);
    if (m_descPool) vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
    for (VkDescriptorPool pool : m_extraPools) vkDestroyDescriptorPool(m_device, pool, nullptr);
    if (m_fence) vkDestroyFence(m_device, m_fence, nullptr);
    if (m_timestamps) vkDestroyQueryPool(m_device, m_timestamps, nullptr);
    if (m_cmdPool) vkDestroyCommandPool(m_device, m_cmdPool, nullptr);
    if (m_ffDevice) {
        av_buffer_unref(&m_ffDevice);  // the device and instance are FFmpeg's
    } else {
        vkDestroyDevice(m_device, nullptr);
        vkDestroyInstance(m_instance, nullptr);
    }
    m_device = VK_NULL_HANDLE;
    m_instance = VK_NULL_HANDLE;
}

// ------------------------------------------------------------ resources

int32_t VkEngine::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required,
                                 VkMemoryPropertyFlags preferred) {
    for (int pass = 0; pass < 2; ++pass) {
        const VkMemoryPropertyFlags want = pass == 0 ? (required | preferred) : required;
        for (uint32_t i = 0; i < m_memProps.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) && (m_memProps.memoryTypes[i].propertyFlags & want) == want)
                return static_cast<int32_t>(i);
        }
    }
    return -1;
}

bool VkEngine::createImage(Image &img, int w, int h, VkFormat fmt, VkImageUsageFlags usage) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ici, nullptr, &img.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(m_device, img.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    const int32_t type = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type < 0) { destroyImage(img); return false; }
    mai.memoryTypeIndex = static_cast<uint32_t>(type);
    if (vkAllocateMemory(m_device, &mai, nullptr, &img.memory) != VK_SUCCESS) { destroyImage(img); return false; }
    vkBindImageMemory(m_device, img.image, img.memory, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_device, &vci, nullptr, &img.view) != VK_SUCCESS) { destroyImage(img); return false; }
    img.format = fmt;
    img.width = w;
    img.height = h;
    img.external = false;
    img.initialised = false;
    return true;
}

bool VkEngine::importDmaBuf(Image &img, const DmaBufFrame &f, const FormatInfo &fi) {
    if (!m_hasDmaBuf || f.planes.size() != 1 || f.modifier == DRM_FORMAT_MOD_INVALID) return false;

    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkSubresourceLayout plane{};
    plane.offset = f.planes[0].offset;
    plane.rowPitch = f.planes[0].stride;
    VkImageDrmFormatModifierExplicitCreateInfoEXT mod{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    mod.drmFormatModifier = f.modifier;
    mod.drmFormatModifierPlaneCount = 1;
    mod.pPlaneLayouts = &plane;
    ext.pNext = &mod;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &ext;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fi.vk;
    ici.extent = {static_cast<uint32_t>(f.width), static_cast<uint32_t>(f.height), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ici, nullptr, &img.image) != VK_SUCCESS) return false;

    const int fd = dup(f.planes[0].fd);
    if (fd < 0) { destroyImage(img); return false; }
    VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (m_getMemoryFdProperties(m_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProps) !=
        VK_SUCCESS) {
        close(fd);
        destroyImage(img);
        return false;
    }
    VkMemoryDedicatedRequirements dedReq{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    req.pNext = &dedReq;
    VkImageMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    reqInfo.image = img.image;
    vkGetImageMemoryRequirements2(m_device, &reqInfo, &req);

    const uint32_t bits = req.memoryRequirements.memoryTypeBits & fdProps.memoryTypeBits;
    const int32_t type = findMemoryType(bits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) { close(fd); destroyImage(img); return false; }

    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = img.image;
    VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    imp.pNext = &ded;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    imp.fd = fd;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &imp;
    mai.allocationSize = req.memoryRequirements.size;
    mai.memoryTypeIndex = static_cast<uint32_t>(type);
    if (vkAllocateMemory(m_device, &mai, nullptr, &img.memory) != VK_SUCCESS) {
        close(fd);  // import failed, the fd is still ours
        destroyImage(img);
        return false;
    }
    if (vkBindImageMemory(m_device, img.image, img.memory, 0) != VK_SUCCESS) { destroyImage(img); return false; }

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fi.vk;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_device, &vci, nullptr, &img.view) != VK_SUCCESS) { destroyImage(img); return false; }
    img.format = fi.vk;
    img.width = f.width;
    img.height = f.height;
    img.external = true;
    img.initialised = false;
    return true;
}

void VkEngine::destroyImage(Image &img) {
    // A device that stopped answering (a fence timed out) may still be using
    // it: kept until the engine goes, after its last wait (and left with the
    // device if that wait gets no answer either, see lastWait).
    if (m_lost && !m_tearingDown && (img.view || img.image || img.memory)) {
        m_doomed.push_back(img);
        img = Image{};
        return;
    }
    if (img.view) vkDestroyImageView(m_device, img.view, nullptr);
    if (img.image) vkDestroyImage(m_device, img.image, nullptr);
    if (img.memory) vkFreeMemory(m_device, img.memory, nullptr);
    img = Image{};
}

bool VkEngine::createBuffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &b.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, b.buffer, &req);
    const int32_t type = hostVisible
        ? findMemoryType(req.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
        : findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type < 0) { destroyBuffer(b); return false; }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = static_cast<uint32_t>(type);
    if (vkAllocateMemory(m_device, &mai, nullptr, &b.memory) != VK_SUCCESS) { destroyBuffer(b); return false; }
    vkBindBufferMemory(m_device, b.buffer, b.memory, 0);
    if (hostVisible && vkMapMemory(m_device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped) != VK_SUCCESS) {
        destroyBuffer(b);
        return false;
    }
    b.size = size;
    return true;
}

void VkEngine::destroyBuffer(Buffer &b) {
    // As destroyImage: it may still be in use.
    if (m_lost && !m_tearingDown && (b.buffer || b.memory)) {
        m_doomedBuffers.push_back(b);
        b = Buffer{};
        return;
    }
    if (b.mapped) vkUnmapMemory(m_device, b.memory);
    if (b.buffer) vkDestroyBuffer(m_device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(m_device, b.memory, nullptr);
    b = Buffer{};
}

// ------------------------------------------------------------ recording

void VkEngine::fail(VkResult r) {
    if (r == VK_ERROR_DEVICE_LOST) m_lost = true;
}

bool VkEngine::begin() {
    if (m_lost) return false;
    if (m_recording) return true;
    settle();  // the command buffer and descriptor pool may still be in use
    if (m_lost) return false;  // and now always will be
    vkResetDescriptorPool(m_device, m_descPool, 0);
    for (VkDescriptorPool pool : m_extraPools) vkResetDescriptorPool(m_device, pool, 0);
    m_extraUsed = 0;
    m_currentPool = m_descPool;
    m_usedSets.clear();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(m_cmd, &bi) != VK_SUCCESS) return false;
    m_recording = true;
    ++m_bufferSerial;
    m_stagingUsed = 0;
    m_timestampsUsed = 0;
    if (m_tickNs > 0) vkCmdResetQueryPool(m_cmd, m_timestamps, 0, kTimestamps);
    return true;
}

void VkEngine::harvestTimings() {
    for (const Timing &t : m_timingsInFlight) {
        uint64_t ticks[2];
        if (vkGetQueryPoolResults(m_device, m_timestamps, t.first, 2, sizeof ticks, ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || ticks[1] < ticks[0])
            continue;
        const double ms = double(ticks[1] - ticks[0]) * m_tickNs / 1e6;
        auto &times = t.wrap ? m_wrapMs : m_filterMs;
        const auto it = times.find(t.source);
        if (it == times.end()) times[t.source] = ms;
        else it->second += (ms - it->second) * 0.1;
    }
    m_timingsInFlight.clear();
}

bool VkEngine::submitRecorded() {
    m_recording = false;
    vkEndCommandBuffer(m_cmd);
    m_timingsInFlight = std::move(m_timings);
    m_timings.clear();
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_cmd;
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    if (!m_waitSems.empty() || !m_signalSems.empty()) {
        timeline.waitSemaphoreValueCount = static_cast<uint32_t>(m_waitValues.size());
        timeline.pWaitSemaphoreValues = m_waitValues.data();
        timeline.signalSemaphoreValueCount = static_cast<uint32_t>(m_signalValues.size());
        timeline.pSignalSemaphoreValues = m_signalValues.data();
        si.pNext = &timeline;
        si.waitSemaphoreCount = static_cast<uint32_t>(m_waitSems.size());
        si.pWaitSemaphores = m_waitSems.data();
        si.pWaitDstStageMask = m_waitStages.data();
        si.signalSemaphoreCount = static_cast<uint32_t>(m_signalSems.size());
        si.pSignalSemaphores = m_signalSems.data();
    }
    vkResetFences(m_device, 1, &m_fence);
    // Lost while recording: nothing it recorded may run.
    VkResult r = VK_ERROR_DEVICE_LOST;
    if (!m_lost) {
        lockQueue();
        r = vkQueueSubmit(m_queue, 1, &si, m_fence);
        unlockQueue();
    }
    if (r != VK_SUCCESS) {
        // The encoders wait for these values: given from here, so none waits
        // for ever on pictures that were never drawn (they get what was there).
        for (size_t i = 0; i < m_signalSems.size(); ++i) {
            VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
            signal.semaphore = m_signalSems[i];
            signal.value = m_signalValues[i];
            vkSignalSemaphore(m_device, &signal);
        }
    }
    m_waitSems.clear(); m_waitValues.clear(); m_waitStages.clear();
    m_signalSems.clear(); m_signalValues.clear();
    // Any failure here leaves the engine in no state to go on (out of memory
    // as much as a lost device): taken as lost, the session makes a new one.
    if (r != VK_SUCCESS) { fail(VK_ERROR_DEVICE_LOST); return false; }
    m_inFlight = true;
    return true;
}

void VkEngine::flush() {
    settle();
    if (!m_recording) return;
    if (!submitRecorded()) return;
    waitForFence();
}

void VkEngine::submit() {
    if (!m_recording) return;
    if (submitRecorded()) m_submitted = true;
}

void VkEngine::settle() {
    if (!m_submitted) return;
    m_submitted = false;
    waitForFence();
}

// A wait that runs out says only that the GPU has not finished: it may be
// behind another program's long job, moving memory, or a software renderer
// compiling a shader in its first dispatch. A GPU that really hung comes back
// as VK_ERROR_DEVICE_LOST from the same wait, once the kernel has ended it, so
// a minute of silence is only the last resort.
void VkEngine::waitForFence() {
    constexpr int kWaits = 12, kSeconds = 5;
    VkResult r = VK_TIMEOUT;
    for (int i = 1; i <= kWaits && r == VK_TIMEOUT; ++i) {
        r = vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, uint64_t(kSeconds) * 1'000'000'000ull);
        if (r == VK_TIMEOUT) qWarning("%s has not answered for %d s.", m_deviceName.c_str(), i * kSeconds);
    }
    // Only the fence's own word ends the work: after a wait that ran out it may
    // still be running, and shutdown() must know which it was.
    m_silent = r == VK_TIMEOUT;
    if (r == VK_SUCCESS) m_inFlight = false;
    if (r != VK_SUCCESS) fail(r == VK_TIMEOUT ? VK_ERROR_DEVICE_LOST : r);
    else harvestTimings();
    for (Readback &rb : m_readbacks) if (rb.id < 0) rb.id = 0;
}

void VkEngine::imageBarrier(Image &img, VkImageLayout from, VkImageLayout to, bool acquireForeign) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                      VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = acquireForeign && m_hasForeignQueue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = acquireForeign && m_hasForeignQueue ? m_family : VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    img.initialised = true;
}

void VkEngine::memoryBarrier() {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                      VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                         &b, 0, nullptr, 0, nullptr);
}

void VkEngine::hostReadBarrier() {
    // The copy made visible to the host's reads once the fence is waited on.
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

VkDescriptorPool VkEngine::makeDescriptorPool() {
    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512},
    };
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 512;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    return vkCreateDescriptorPool(m_device, &dpi, nullptr, &pool) == VK_SUCCESS ? pool : VK_NULL_HANDLE;
}

VkDescriptorSet VkEngine::allocateSet(const Pipeline &p) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_currentPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &p.layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    // This pool full: the next, made if need be, and it stays the one to ask.
    while (vkAllocateDescriptorSets(m_device, &ai, &set) != VK_SUCCESS) {
        if (m_extraUsed == m_extraPools.size()) {
            const VkDescriptorPool more = makeDescriptorPool();
            // Out of memory: what is recorded now is never submitted (see
            // submitRecorded), and the session makes the engine again.
            if (!more) { fail(VK_ERROR_DEVICE_LOST); return VK_NULL_HANDLE; }
            m_extraPools.push_back(more);
        }
        ai.descriptorPool = m_currentPool = m_extraPools[m_extraUsed++];
    }
    return set;
}

VkDescriptorSet VkEngine::descriptorSet(const Pipeline &p, VkImageView sampled, VkImageView storage,
                                        VkBuffer buffer, VkDeviceSize bufferSize, VkImageView sampled2) {
    const VkDescriptorSet set = allocateSet(p);
    if (!set) return VK_NULL_HANDLE;

    VkDescriptorImageInfo sampledInfo{m_linear, sampled, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo storageInfo{VK_NULL_HANDLE, storage, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bufferInfo{buffer, 0, bufferSize};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &sampledInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    if (storage) {
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &storageInfo;
    } else {
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &bufferInfo;
    }
    VkWriteDescriptorSet all[3] = {writes[0], writes[1], {}};
    VkDescriptorImageInfo secondInfo{m_linear, sampled2, VK_IMAGE_LAYOUT_GENERAL};
    if (sampled2) {
        all[2] = writes[0];
        all[2].dstBinding = 2;
        all[2].pImageInfo = &secondInfo;
    }
    vkUpdateDescriptorSets(m_device, sampled2 ? 3 : 2, all, 0, nullptr);
    return set;
}

void VkEngine::setSourceDeinterlace(int id, DeinterlaceMode mode) {
    const auto it = m_sources.find(id);
    if (it == m_sources.end() || it->second.deinterlace == mode) return;
    it->second.deinterlace = mode;
    resetSourceHistory(id);
}
void VkEngine::resetSourceHistory(int id) {
    const auto it = m_sources.find(id); if (it == m_sources.end()) return;
    auto &s = it->second;
    s.previousValid = false; s.resetNext = true; s.fieldReady = false;
    s.historyValid = false; s.pendingProcess = true;
}
void VkEngine::beginFrame(Source &s, int w, int h) {
    s.previousValid = s.deinterlace != DeinterlaceMode::Off && s.hasFrame && !s.resetNext && s.work.width == w && s.work.height == h;
    if (s.deinterlace != DeinterlaceMode::Off) std::swap(s.work, s.previous);
    s.resetNext = false; s.fieldReady = false;
}
void VkEngine::finishFrame(Source &s) {
    s.hasFrame = true; s.pendingProcess = true; ++s.generation;
    if (s.deinterlace == DeinterlaceMode::Off) { recordDenoise(s); s.pendingProcess = false; }
}
void VkEngine::renderSourceField(int id, int field, bool firstInFrame) {
    const auto it = m_sources.find(id); if (it == m_sources.end() || m_lost) return;
    auto &s = it->second; if (!s.hasFrame) return;
    field = field == 1 ? 1 : 0;
    if (s.deinterlace == DeinterlaceMode::Off) {
        if (!s.pendingProcess) return;
        recordDenoise(s);
    } else {
        if (s.fieldReady && s.field == field && s.firstField == firstInFrame && !s.pendingProcess) return;
        if (!recordDeinterlace(s, field, firstInFrame)) return;
        recordDenoise(s);
    }
    s.pendingProcess = false; ++s.generation;
}
bool VkEngine::recordDeinterlace(Source &s, int field, bool first) {
    auto &out = s.fieldFrame;
    if (out.width != s.work.width || out.height != s.work.height) {
        flush(); destroyImage(out);
        if (!createImage(out,s.work.width,s.work.height,VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return false;
    }
    if (!begin()) return false;
    if (!out.initialised) imageBarrier(out,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
    const auto &history = s.previousValid ? s.previous : s.work;
    const auto set = descriptorSet(m_deinterlace,s.work.view,out.view,VK_NULL_HANDLE,0,history.view);
    if (!set) return false;
    const DeinterlacePush push{s.work.width,s.work.height,int(s.deinterlace),field,int(first),int(s.previousValid)};
    vkCmdBindPipeline(m_cmd,VK_PIPELINE_BIND_POINT_COMPUTE,m_deinterlace.pipeline);
    vkCmdBindDescriptorSets(m_cmd,VK_PIPELINE_BIND_POINT_COMPUTE,m_deinterlace.pipelineLayout,0,1,&set,0,nullptr);
    vkCmdPushConstants(m_cmd,m_deinterlace.pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,&push);
    vkCmdDispatch(m_cmd,groups(s.work.width,16),groups(s.work.height,16),1);
    memoryBarrier();
    s.fieldReady = true; s.field = field; s.firstField = first;
    return true;
}

void VkEngine::recordConvert(Source &s, const Image &from, bool forceOpaque, bool premultiplied, bool limitedRange, bool linearAlpha, bool yInvert, BufferTransform transform) {
    const int width=swapsAxes(transform) ? from.height : from.width;
    const int height=swapsAxes(transform) ? from.width : from.height;
    beginFrame(s, width, height);
    if (s.work.width != width || s.work.height != height) {
        // Resizing while recording is fine: the old image is only destroyed
        // after this flush completes (we flush first).
        flush();
        destroyImage(s.work);
        // Out of memory for it: the source shows nothing rather than an image that is not there.
        if (!createImage(s.work, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
            s.hasFrame = false;
            return;
        }
        if (!begin()) { s.hasFrame = false; return; }
    }
    if (!s.work.initialised) imageBarrier(s.work, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    const VkDescriptorSet set = descriptorSet(m_convert, from.view, s.work.view, VK_NULL_HANDLE, 0);
    if (!set) return;
    const ConvertPush push{width, height, (forceOpaque ? 1 : 0) | (premultiplied ? 2 : 0) | (limitedRange ? 4 : 0) | (linearAlpha ? 0 : 8) | (yInvert ? 16 : 0) | (int(transform) << 5)};
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_convert.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_convert.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, m_convert.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(width, 16), groups(height, 16), 1);
    memoryBarrier();
    finishFrame(s);
}

void VkEngine::recordDenoise(Source &s) {
    if (s.denoise <= 0.f) {
        s.historyValid = false;
        return;
    }
    if (!begin()) return;
    const auto &cur = s.processed();
    const int next = 1 - s.cleanIndex;
    for (Image &img : s.clean) {
        if (img.width == cur.width && img.height == cur.height) continue;
        flush();
        destroyImage(img);
        s.historyValid = false;
        if (!createImage(img, cur.width, cur.height, VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            return;  // shown without noise reduction
        if (!begin()) return;
    }
    for (Image &img : s.clean)
        if (!img.initialised) imageBarrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    const DenoiseParams dp = denoiseParams(s.denoise);
    const DenoisePush push{cur.width, cur.height, dp.sigma, dp.threshold, dp.keepMin, s.historyValid ? 0 : 1};
    const VkDescriptorSet set =
        descriptorSet(m_denoise, cur.view, s.clean[next].view, VK_NULL_HANDLE, 0, s.clean[s.cleanIndex].view);
    if (!set) return;
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, m_denoise.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(static_cast<uint32_t>(cur.width), 16), groups(static_cast<uint32_t>(cur.height), 16), 1);
    memoryBarrier();
    s.cleanIndex = next;
    s.historyValid = true;
}

// ------------------------------------------------------------- public ops

// Room in the staging buffer for one memory frame, after those this command
// buffer has already taken in: a tick's uploads then go to the GPU together,
// where each used to wait for the one before to finish. True with the buffer
// recording and `offset` the frame's place; the buffer is free again from the
// next command buffer's first upload (begin() waits for the work that read it).
bool VkEngine::stagingRoom(VkDeviceSize bytes, VkDeviceSize *offset) {
    constexpr VkDeviceSize kMost = VkDeviceSize(256) << 20;  // of frames waiting in it at once
    if (!begin()) return false;
    VkDeviceSize at = (m_stagingUsed + 63) / 64 * 64;
    if (at > 0 && at + bytes > m_staging.size) {
        // No room after the others. They go first, and the buffer grows to hold
        // them all another time, up to a point: past it they go in turns.
        flush();
        const VkDeviceSize all = at + bytes < kMost ? at + bytes : kMost;
        if (all > bytes) ensureBuffer(m_staging, all, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        at = 0;
    }
    // Growing it here is safe: at nought, nothing recorded or sent reads it.
    if (!ensureBuffer(m_staging, at + bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true) || !begin()) return false;
    *offset = at;
    m_stagingUsed = at + bytes;
    return true;
}

bool VkEngine::updateSource(int id, const CpuFrame &f) {
    auto it = m_sources.find(id);
    if (it == m_sources.end() || !f.data || f.width <= 0 || f.height <= 0 || m_lost) return false;
    Source &s = it->second;
    const FormatInfo fi = formatForPixel(f.format);

    if (s.upload.width != f.width || s.upload.height != f.height || s.upload.format != fi.vk) {
        flush();  // earlier work may still read the one it had
        destroyImage(s.upload);
        if (!createImage(s.upload, f.width, f.height, fi.vk,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            return false;
    }
    const VkDeviceSize packed = static_cast<VkDeviceSize>(f.width) * f.height * 4;
    VkDeviceSize staged = 0;
    if (!stagingRoom(packed, &staged)) return false;
    uint8_t *dst = static_cast<uint8_t *>(m_staging.mapped) + staged;
    const size_t rowBytes = static_cast<size_t>(f.width) * 4;
    for (int y = 0; y < f.height; ++y)
        std::memcpy(dst + y * rowBytes, f.data + static_cast<size_t>(y) * f.stride, rowBytes);

    if (!begin()) return false;
    imageBarrier(s.upload, s.upload.initialised ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = staged;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(f.width), static_cast<uint32_t>(f.height), 1};
    vkCmdCopyBufferToImage(m_cmd, m_staging.buffer, s.upload.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    imageBarrier(s.upload, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    recordConvert(s, s.upload, !fi.alpha, f.premultiplied, f.limitedRange, f.linearAlpha);
    return true;
}

bool VkEngine::updateSource(int id, const YuvInput &f) {
    auto it = m_sources.find(id);
    if (it == m_sources.end() || !f.plane[0] || f.width <= 0 || f.height <= 0 || m_lost) return false;
    if (f.layout != YuvLayout::YUY2 &&
        (!f.plane[1] || (f.layout != YuvLayout::NV12 && !f.plane[2])))
        return false;
    if (f.depth < 8 || f.depth > 16) return false;
    Source &s = it->second;
    const YuvRows rows = yuvRows(f);
    const int sb = sampleBytes(f), cw = (f.width + 1) / 2 * sb, ch = (f.height + 1) / 2, fw = f.width * sb;

    if (s.upload.width != rows.rowBytes || s.upload.height != rows.rows || s.upload.format != VK_FORMAT_R8_UNORM) {
        flush();  // earlier work may still read the one it had
        destroyImage(s.upload);
        if (!createImage(s.upload, rows.rowBytes, rows.rows, VK_FORMAT_R8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            return false;
    }
    const VkDeviceSize packed = static_cast<VkDeviceSize>(rows.rowBytes) * rows.rows;
    VkDeviceSize staged = 0;
    if (!stagingRoom(packed, &staged)) return false;
    uint8_t *dst = static_cast<uint8_t *>(m_staging.mapped) + staged;
    auto copyRows = [&](int firstRow, const uint8_t *src, int stride, int count, int bytes) {
        for (int r = 0; r < count; ++r)
            std::memcpy(dst + static_cast<size_t>(firstRow + r) * rows.rowBytes, src + static_cast<size_t>(r) * stride,
                        static_cast<size_t>(bytes));
    };
    switch (f.layout) {
    case YuvLayout::NV12:
        copyRows(0, f.plane[0], f.stride[0], f.height, fw);
        copyRows(f.height, f.plane[1], f.stride[1], ch, cw * 2);
        break;
    case YuvLayout::I420:
        copyRows(0, f.plane[0], f.stride[0], f.height, fw);
        copyRows(f.height, f.plane[1], f.stride[1], ch, cw);
        copyRows(f.height + ch, f.plane[2], f.stride[2], ch, cw);
        break;
    case YuvLayout::I422:
    case YuvLayout::I444:
        copyRows(0, f.plane[0], f.stride[0], f.height, fw);
        copyRows(f.height, f.plane[1], f.stride[1], f.height, f.layout == YuvLayout::I444 ? fw : cw);
        copyRows(f.height * 2, f.plane[2], f.stride[2], f.height, f.layout == YuvLayout::I444 ? fw : cw);
        break;
    case YuvLayout::YUY2:
        copyRows(0, f.plane[0], f.stride[0], f.height, cw * 4);
        break;
    }
    const int alphaRow = f.alpha ? rows.rows - f.height : -1;
    if (f.alpha) copyRows(alphaRow, f.alpha, f.alphaStride, f.height, fw);

    if (!begin()) return false;
    imageBarrier(s.upload, s.upload.initialised ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = staged;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(rows.rowBytes), static_cast<uint32_t>(rows.rows), 1};
    vkCmdCopyBufferToImage(m_cmd, m_staging.buffer, s.upload.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    imageBarrier(s.upload, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

    beginFrame(s, f.width, f.height);
    if (s.work.width != f.width || s.work.height != f.height) {
        flush();
        destroyImage(s.work);
        if (!createImage(s.work, f.width, f.height, VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
            s.hasFrame = false;
            return false;
        }
        if (!begin()) return false;
    }
    if (!s.work.initialised) imageBarrier(s.work, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    float kr, kb;
    yuvCoefficients(f.matrix, &kr, &kb);
    const VkDescriptorSet set = descriptorSet(m_yuvIn, s.upload.view, s.work.view, VK_NULL_HANDLE, 0);
    if (!set) return false;
    const InputParams light = inputParams(f, m_levels);
    YuvInPush push{f.width, f.height, static_cast<int32_t>(f.layout), f.fullRange ? 1 : 0, kr, kb,
        (f.interlaced || s.deinterlace != DeinterlaceMode::Off) ? 1 : 0, alphaRow, f.premultiplied ? 1 : 0, f.linearAlpha ? 1 : 0,
        f.depth, f.depth > 8 && f.msb ? 16 - f.depth : 0, light.kind, light.wide, light.scale, light.gamma1, {}};
    std::copy_n(light.gamut, 9, push.gamut);
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_yuvIn.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_yuvIn.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, m_yuvIn.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(static_cast<uint32_t>(f.width), 16), groups(static_cast<uint32_t>(f.height), 16), 1);
    memoryBarrier();
    finishFrame(s);
    return true;
}

bool VkEngine::updateSource(int id, const DmaBufFrame &f) {
    auto it = m_sources.find(id);
    if (it == m_sources.end() || m_lost || !m_hasDmaBuf || uint32_t(f.transform)>7) return false;
    Source &s = it->second;
    const FormatInfo fi = formatForFourcc(f.fourcc);
    if (fi.vk == VK_FORMAT_UNDEFINED) return false;

    Image *img = nullptr;
    auto cached = f.token ? s.imports.find(f.token) : s.imports.end();
    if (cached != s.imports.end() && cached->second.width == f.width && cached->second.height == f.height &&
        cached->second.format == fi.vk) {
        img = &cached->second;
    } else {
        if (cached != s.imports.end()) {
            flush();
            destroyImage(cached->second);
            s.imports.erase(cached);
        }
        // Resize/reconnect must not grow the import cache indefinitely.
        if (s.imports.size() >= 8) {
            flush();
            for (auto &entry : s.imports) destroyImage(entry.second);
            s.imports.clear();
        }
        Image fresh;
        if (!importDmaBuf(fresh, f, fi)) return false;
        if (!f.token) {
            // No stable identity: import, use once, throw away after the flush.
            if (!begin()) { destroyImage(fresh); return false; }
            imageBarrier(fresh, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, true);
            recordConvert(s, fresh, !fi.alpha, f.premultiplied, false, true, f.yInvert, f.transform);
            flush();
            destroyImage(fresh);
            return !m_lost;
        }
        fresh.owner = f.owner;
        img = &s.imports.emplace(f.token, fresh).first->second;
    }
    if (!begin()) return false;
    // The compositor wrote this buffer since we last read it: acquire it again.
    imageBarrier(*img, img->initialised ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_GENERAL, true);
    recordConvert(s, *img, !fi.alpha, f.premultiplied, false, true, f.yInvert, f.transform);
    return true;
}

std::vector<uint64_t> VkEngine::importModifiers(uint32_t fourcc) const {
    std::vector<uint64_t> out;
    if (!m_hasDmaBuf) return out;
    const FormatInfo fi = formatForFourcc(fourcc);
    if (fi.vk == VK_FORMAT_UNDEFINED) return out;

    VkDrmFormatModifierPropertiesListEXT list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    props.pNext = &list;
    vkGetPhysicalDeviceFormatProperties2(m_physical, fi.vk, &props);
    std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = mods.data();
    vkGetPhysicalDeviceFormatProperties2(m_physical, fi.vk, &props);

    for (const auto &m : mods) {
        if (m.drmFormatModifierPlaneCount != 1) continue;
        if (!(m.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) continue;

        VkPhysicalDeviceExternalImageFormatInfo extInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        extInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
        modInfo.pNext = &extInfo;
        modInfo.drmFormatModifier = m.drmFormatModifier;
        modInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        info.pNext = &modInfo;
        info.format = fi.vk;
        info.type = VK_IMAGE_TYPE_2D;
        info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        VkExternalImageFormatProperties extProps{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 outProps{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        outProps.pNext = &extProps;
        if (vkGetPhysicalDeviceImageFormatProperties2(m_physical, &info, &outProps) != VK_SUCCESS) continue;
        if (!(extProps.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
            continue;
        out.push_back(m.drmFormatModifier);
    }
    return out;
}

void VkEngine::composite(const std::vector<Layer> &layers, const Color &bg) { compositeTo(m_canvas, -1, layers, bg); }

void VkEngine::compositeInto(int target, int width, int height, const std::vector<Layer> &layers, const Color &bg) {
    auto it = m_sources.find(target);
    if (it == m_sources.end() || width <= 0 || height <= 0 || m_lost) return;
    Source &s = it->second;
    // Past what the device makes: not drawn, and not asked of it again every frame.
    if (uint32_t(width) > m_maxImage || uint32_t(height) > m_maxImage) {
        s.hasFrame = false;
        return;
    }
    if (s.work.width != width || s.work.height != height || !s.work.clearable) {
        flush();
        destroyImage(s.work);
        if (!createImage(s.work, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            s.hasFrame = false;
            return;
        }
        s.work.clearable = true;
    }
    s.denoise = 0.f;  // a drawn studio is not noisy
    compositeTo(s.work, target, layers, bg);
    s.hasFrame = true; ++s.generation;
}

void VkEngine::transitionInto(int target, int width, int height, int from, int to, const TransitionStyle &style, float progress) {
    if (target == from || target == to || width <= 0 || height <= 0 || m_lost) return;
    auto dst = m_sources.find(target), a = m_sources.find(from), b = m_sources.find(to);
    if (dst == m_sources.end() || a == m_sources.end() || b == m_sources.end() || !a->second.hasFrame || !b->second.hasFrame) return;
    Source &s = dst->second;
    if (s.work.width != width || s.work.height != height) {
        flush(); destroyImage(s.work);
        if (!createImage(s.work, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            s.hasFrame = false;
            return;
        }
        s.work.clearable = true;
    }
    if (!begin()) return;
    imageBarrier(s.work, s.work.initialised ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    const auto set = descriptorSet(m_transition, shownOf(a->second).view, s.work.view, VK_NULL_HANDLE, 0, shownOf(b->second).view);
    if (!set) return;
    const TransitionPush push{width, height, int32_t(style.type), std::clamp(progress, 0.f, 1.f),
                              int32_t(style.side), int32_t(style.mask), style.invert ? 1 : 0, style.softness,
                              style.colourPoint, {0, 0, 0}, {style.colour.r, style.colour.g, style.colour.b, style.colour.a},
                              {style.matteRect[0], style.matteRect[1], style.matteRect[2], style.matteRect[3]}};
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_transition.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_transition.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, m_transition.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, (width + 15) / 16, (height + 15) / 16, 1);
    imageBarrier(s.work, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    s.hasFrame = true; s.denoise = 0.f; ++s.generation;
}

void VkEngine::compositeTo(Image &into, int skip, const std::vector<Layer> &layers, const Color &bg) {
    // Light wrap's pictures, once nothing has drawn with them for a while:
    // counted in composites, as the engine reads no clock of its own.
    ++m_composites;
    const auto idle = [&](const Wrap &w) { return m_composites - w.used > kWrapIdleComposites; };
    if (std::any_of(m_wraps.begin(), m_wraps.end(), idle)) {
        flush();
        std::erase_if(m_wraps, [&](Wrap &w) {
            if (!idle(w)) return false;
            for (Image &img : w.images) destroyImage(img);
            return true;
        });
    }
    if (!into.image || !begin()) return;
    clearTo(into, bg);
    for (const Layer &l : layers) {
        auto it = m_sources.find(l.source);
        if (l.source == skip || it == m_sources.end() || !it->second.hasFrame || l.w <= 0 || l.h <= 0) continue;
        const VideoFilter *wrap = it->second.filtered && !it->second.hide ? lightWrapOf(it->second.filters) : nullptr;
        if (!wrap) { drawLayer(into, shownOf(it->second), l); continue; }
        if (!begin()) return;
        // Timed, as the source's filters are, when it all goes into one command buffer.
        uint32_t first = 0;
        const uint64_t buffer = m_bufferSerial;
        const bool timed = m_tickNs > 0 && m_timestampsUsed + 2 <= kTimestamps;
        if (timed) {
            first = m_timestampsUsed;
            m_timestampsUsed += 2;
            vkCmdWriteTimestamp(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_timestamps, first);
        }
        if (!wrapLayer(into, it->second.shown(), l, *wrap)) drawLayer(into, it->second.shown(), l);
        else if (timed && buffer == m_bufferSerial && m_recording) {
            vkCmdWriteTimestamp(m_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timestamps, first + 1);
            m_timings.push_back({l.source, first, true});
        }
    }
}

// A layer with light wrap (lightWrapParams in frame.h): in its region, drawn
// alone on the picture's grid (unless it is its own picture, whole), the
// light under it brought on, then drawn in as it would have been. False,
// with nothing drawn, when there is no memory for its pictures.
bool VkEngine::wrapLayer(Image &into, const Image &shown, const Layer &l, const VideoFilter &f) {
    const PixelBounds b = lightWrapRegion(f, l, into.width, into.height);
    if (b.empty()) return true;
    const int scale = lightWrapScale(f, into.height);
    const int bw = b.x1 - b.x0, bh = b.y1 - b.y0, hw = (bw + scale - 1) / scale, hh = (bh + scale - 1) / scale;
    // Pictures for each region size. A few are kept: past those the one longest
    // out of use makes way, but never one in use just now, or five wrapped layers
    // of different sizes would make their pictures anew every frame.
    auto it = std::find_if(m_wraps.begin(), m_wraps.end(), [&](const Wrap &w) { return w.width == bw && w.height == bh && w.scale == scale; });
    if (it == m_wraps.end()) {
        if (m_wraps.size() >= kWrapSetsKept) {
            const auto oldest = std::min_element(m_wraps.begin(), m_wraps.end(), [](const Wrap &a, const Wrap &c) { return a.used < c.used; });
            if (m_composites - oldest->used > kWrapBusyComposites) {
                flush();
                for (Image &img : oldest->images) destroyImage(img);
                m_wraps.erase(oldest);
            }
        }
        m_wraps.push_back({bw, bh, scale, {}, m_composites});
        it = m_wraps.end() - 1;
    }
    it->used = m_composites;
    Image &drawn = it->images[0], &lit = it->images[1], &light = it->images[2], &across = it->images[3];
    const bool whole = layerIsWhole(l, shown.width, shown.height, into.width, into.height);
    if ((!whole && !filterImage(drawn, bw, bh)) || !filterImage(lit, bw, bh) || !filterImage(light, hw, hh) ||
        !filterImage(across, hw, hh) || !begin())
        return false;
    if (!whole) {
        clearTo(drawn, {0, 0, 0, 0});
        Layer plain = l;
        plain.x -= float(b.x0); plain.y -= float(b.y0);
        plain.blend = BlendMode::Normal;
        plain.blendSpace = BlendSpace::Linear;
        drawLayer(drawn, shown, plain);
    }
    const Image &alone = whole ? shown : drawn;
    recordFilter(into, light, &alone, lightWrapParams(f, 0, into.height, b.x0, b.y0, hw, hh));
    recordFilter(light, across, nullptr, lightWrapParams(f, 1, into.height, b.x0, b.y0, hw, hh));
    recordFilter(across, light, nullptr, lightWrapParams(f, 2, into.height, b.x0, b.y0, hw, hh));
    recordFilter(alone, lit, &light, lightWrapParams(f, 3, into.height, b.x0, b.y0, hw, hh));
    Layer region;
    region.x = float(b.x0); region.y = float(b.y0); region.w = float(bw); region.h = float(bh);
    region.filter = ScaleFilter::Point;
    region.blend = l.blend;
    region.blendSpace = l.blendSpace;
    drawLayer(into, lit, region);
    return true;
}

void VkEngine::clearTo(Image &into, const Color &bg) {
    imageBarrier(into, into.initialised ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue clear{};
    clear.float32[0] = bg.r;
    clear.float32[1] = bg.g;
    clear.float32[2] = bg.b;
    clear.float32[3] = bg.a;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(m_cmd, into.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    imageBarrier(into, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
}

void VkEngine::drawLayer(Image &into, const Image &shown, const Layer &l) {
    if (!begin()) return;  // a light wrap with no memory has flushed and given up
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_composite.pipeline);
    {
        const PixelBounds b = layerBounds(l, into.width, into.height);
        if (b.empty()) return;
        const VkDescriptorSet set = descriptorSet(m_composite, shown.view, into.view, VK_NULL_HANDLE, 0);
        if (!set) return;
        const float fx = std::fabs(l.u1 - l.u0) * shown.width / l.w, fy = std::fabs(l.v1 - l.v0) * shown.height / l.h;
        const CompositePush push{l.x, l.y, l.w, l.h, l.u0, l.v0, l.u1, l.v1, std::cos(l.rotation), std::sin(l.rotation),
                                 b.x0, b.y0, into.width, into.height, l.opacity,
                                 static_cast<int32_t>(resolveFilter(l.filter, fx, fy)), fx, fy,
                                 static_cast<int32_t>(l.blend), static_cast<int32_t>(l.blendSpace), 0, 0};
        vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_composite.pipelineLayout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(m_cmd, m_composite.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        vkCmdDispatch(m_cmd, groups(static_cast<uint32_t>(b.x1 - b.x0), 16),
                      groups(static_cast<uint32_t>(b.y1 - b.y0), 16), 1);
        memoryBarrier();
    }
}

// ------------------------------------------------------------ video filters

bool VkEngine::createNothing(std::string *failure) {
    if (filterImage(m_nothing, 1, 1) && begin()) {
        clearTo(m_nothing, {0, 0, 0, 0});
        flush();
        if (!m_lost) return true;
    }
    if (failure) *failure = m_lost ? "the device stopped answering as the engine started" : "no memory on the device for the engine's first picture";
    return false;
}

bool VkEngine::filterImage(Image &img, int w, int h, VkFormat format) {
    if (img.image && img.width == w && img.height == h && img.format == format) return true;
    flush();
    destroyImage(img);
    if (!createImage(img, w, h, format,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;
    img.clearable = true;
    if (!begin()) return false;
    imageBarrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    return true;
}

// A LUT or mask picture, rarely: flushed straight away so the staging buffer is free again.
bool VkEngine::uploadAux(Image &img, VkFormat format, int w, int h, const void *data, size_t bytes) {
    flush();
    destroyImage(img);
    if (!createImage(img, w, h, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !ensureBuffer(m_auxStaging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true) || !begin())
        return false;
    std::memcpy(m_auxStaging.mapped, data, bytes);
    imageBarrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {uint32_t(w), uint32_t(h), 1};
    vkCmdCopyBufferToImage(m_cmd, m_auxStaging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    imageBarrier(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    flush();
    return !m_lost;
}

// RemoveBackground: the network run on `in` into the step's matte (its aux
// picture). The first matte is made whole; after that a new picture starts a
// run of `parts` parts, one a frame, the matte changing when the last is done.
bool VkEngine::recordMatte(const Image &in, Source::Step &st, const FilterParams &q, int parts, bool fresh, FilterFailure &why) {
    const matting::Model &m = matting::model();
    if (!m.ok) { why = FilterFailure::Model; return false; }
    const bool samePlan = st.plan.ok && st.plan.width == q.ix && st.plan.height == q.iy && st.plan.ends.size() == size_t(std::clamp(parts, 1, 8)) &&
                          st.plan.passes.front().inW == in.width && st.plan.passes.front().inH == in.height;
    if (!samePlan) {
        st.plan = matting::plan(m, q.ix, q.iy, in.width, in.height, parts);
        st.auxOf = nullptr;
        st.part = 0;
        st.history = false;
    }
    const VkDeviceSize weights = m.values.size() * sizeof(float), arena = st.plan.arena * sizeof(float);
    if (!st.plan.ok || weights > m_maxStorage || arena > m_maxStorage) { why = FilterFailure::Device; return false; }
    if (!m_nnWeights.buffer) {
        // Once: through a buffer of its own, so nothing else waits on the staging one.
        flush();
        Buffer host;
        const bool made = createBuffer(m_nnWeights, weights, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) &&
                          createBuffer(host, weights, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true) && begin();
        if (made) {
            std::memcpy(host.mapped, m.values.data(), weights);
            const VkBufferCopy region{0, 0, weights};
            vkCmdCopyBuffer(m_cmd, host.buffer, m_nnWeights.buffer, 1, &region);
            memoryBarrier();
            flush();
        }
        destroyBuffer(host);
        if (!made || m_lost) { destroyBuffer(m_nnWeights); return false; }
    }
    if (st.arena.size < arena) {
        flush();  // the smaller one may still be in use
        if (!ensureBuffer(st.arena, arena, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false)) return false;
    }
    if (!filterImage(st.aux, q.ix, q.iy)) return false;
    size_t first = 0, last = st.plan.passes.size();
    if (st.auxOf != &m) st.part = 0;
    else if (st.part > 0) { first = st.plan.ends[st.part - 1]; last = st.plan.ends[st.part]; st.part = st.part + 1 < st.plan.ends.size() ? st.part + 1 : 0; }
    else if (fresh) { last = st.plan.ends[0]; st.part = st.plan.ends.size() > 1 ? 1 : 0; }
    else return true;
    if (!begin()) return false;
    const VkDescriptorSet set = allocateSet(m_nn);
    if (!set) return false;
    const VkDescriptorImageInfo source{m_linear, in.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo matte{VK_NULL_HANDLE, st.aux.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo buffers[2] = {{st.arena.buffer, 0, arena}, {m_nnWeights.buffer, 0, weights}};
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &source;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &buffers[0];
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &buffers[1];
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[3].pImageInfo = &matte;
    vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_nn.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_nn.pipelineLayout, 0, 1, &set, 0, nullptr);
    for (size_t index = first; index < last; ++index) {
        const matting::Pass &pass = st.plan.passes[index];
        // A thread a pixel (64 a workgroup), or a workgroup a channel.
        uint32_t g[3] = {groups(uint32_t(pass.outW * pass.outH), 64), 1, 1};
        switch (pass.op) {
        case matting::NormStats: g[0] = uint32_t(pass.splits); g[1] = uint32_t(pass.cout - pass.actUntil); break;
        case matting::NormMerge: g[0] = groups(uint32_t(pass.cout - pass.actUntil), 64); break;
        case matting::Pool: g[0] = uint32_t(pass.c0); break;
        case matting::Conv:
            // Eight channels a row of workgroups; a full one: four pixels in a
            // row a thread (PX in nn.comp), and with fewer runs than 64 the
            // eights in x instead.
            g[1] = groups(uint32_t(pass.cout), 8);
            if (!pass.depthwise) {
                const uint32_t runs = uint32_t((pass.outW + 3) / 4 * pass.outH);
                g[0] = runs < 64 ? groups(runs * g[1], 64) : groups(runs, 64);
                if (runs < 64) g[1] = 1;
            }
            g[2] = uint32_t(pass.splits);
            break;
        case matting::Sum: g[1] = uint32_t(pass.cout); break;
        case matting::Resize:
        case matting::Scale: g[1] = uint32_t(pass.c0); break;
        case matting::NormApply: g[1] = uint32_t(pass.cout - pass.actUntil); break;
        default: break;
        }
        vkCmdPushConstants(m_cmd, m_nn.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pass, &pass);
        vkCmdDispatch(m_cmd, g[0], g[1], g[2]);
        memoryBarrier();
    }
    st.auxOf = &m;
    return true;
}

// RemoveBackground after its network (the matte in st.aux): steadied and
// brought to the picture's edges (matteParams in frame.h), the picture cut
// out by it, and with blurBackground put over itself blurred.
bool VkEngine::recordGuided(const Image &in, Source::Step &st, const VideoFilter &f, const FilterParams &q) {
    const int w = in.width, h = in.height;
    for (Image &img : st.guide)
        if (!filterImage(img, q.ix, q.iy, VK_FORMAT_R32G32B32A32_SFLOAT)) return false;
    if (f.blurBackground && (!filterImage(st.back[0], (w + 3) / 4, (h + 3) / 4) || !filterImage(st.back[1], (w + 3) / 4, (h + 3) / 4) ||
                             !filterImage(st.mid, w, h)))
        return false;
    if (!begin()) return false;
    Image &history = st.guide[0], &last = st.guide[1 + st.flip], &now = st.guide[2 - st.flip], &spare = st.guide[3], &blurred = st.guide[4],
          &fit = st.guide[5];
    auto pass = [&](MattePass p) { return matteParams(f, p, w, h, st.history); };
    recordFilter(st.aux, history, &last, pass(MattePass::History));
    recordFilter(in, now, &history, pass(MattePass::Stats));
    recordFilter(now, spare, nullptr, pass(MattePass::Across));
    recordFilter(spare, blurred, nullptr, pass(MattePass::Down));
    recordFilter(blurred, fit, nullptr, pass(MattePass::Coefficients));
    recordFilter(fit, spare, nullptr, pass(MattePass::Across));
    recordFilter(spare, blurred, nullptr, pass(MattePass::Down));
    recordFilter(blurred, fit, &now, pass(MattePass::Pack));
    Image &cut = f.blurBackground ? st.mid : st.out;
    recordFilter(in, cut, &fit, q);
    if (f.blurBackground) {
        recordFilter(in, st.back[0], nullptr, pass(MattePass::BackDown));
        recordFilter(st.back[0], st.back[1], nullptr, pass(MattePass::BackAcross));
        recordFilter(st.back[1], st.back[0], nullptr, pass(MattePass::BackDownBlur));
        recordFilter(cut, st.out, &st.back[0], pass(MattePass::BackOver));
    }
    st.flip ^= 1;
    st.history = true;
    return true;
}

void VkEngine::recordFilter(const Image &in, Image &out, const Image *aux, const FilterParams &q) {
    if (!begin()) return;
    // Into a 32-bit picture: the same shader, built to write those.
    const Pipeline &filter = out.format == VK_FORMAT_R32G32B32A32_SFLOAT ? m_filter32 : m_filter;
    const VkDescriptorSet set = descriptorSet(filter, in.view, out.view, VK_NULL_HANDLE, 0, aux ? aux->view : in.view);
    if (!set) return;
    FilterPush push{in.width, in.height, out.width, out.height, q.kind, q.mode, q.ix, q.iy, {}};
    std::memcpy(push.p, q.p, sizeof push.p);
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, filter.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, filter.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, filter.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(uint32_t(out.width), 16), groups(uint32_t(out.height), 16), 1);
    memoryBarrier();
}

void VkEngine::releaseSteps(Source &s) {
    for (auto &st : s.steps) {
        destroyImage(st.out);
        destroyImage(st.aux);
        destroyImage(st.mid);
        destroyBuffer(st.arena);
        for (Image &img : st.guide) destroyImage(img);
        for (Image &img : st.back) destroyImage(img);
        for (auto &h : st.held) destroyImage(h.first);
        for (auto &img : st.spare) destroyImage(img);
    }
    s.steps.clear();
}

void VkEngine::setSourceFilters(int id, const std::vector<VideoFilter> &filters) {
    const auto it = m_sources.find(id);
    if (it == m_sources.end() || it->second.filters == filters) return;
    flush();
    m_filterMs.erase(id);  // another chain: measured afresh
    m_wrapMs.erase(id);
    Source &s = it->second;
    std::vector<Source::Step> next(filters.size());
    for (size_t i = 0; i < filters.size(); ++i)
        if (filterStepKept(s.filters, filters, i)) next[i] = std::exchange(s.steps[i], {});
    releaseSteps(s);  // the rest
    s.filters = filters;
    s.steps = std::move(next);
    s.filtered = false;
    s.failure = FilterFailure::None;
}

void VkEngine::renderSourceFilters(int id, double seconds) {
    const auto it = m_sources.find(id);
    if (it == m_sources.end() || m_lost) return;
    Source &s = it->second;
    if (s.filters.empty() || !s.hasFrame || !s.base().image) { s.filtered = false; return; }
    if (s.failure != FilterFailure::None && seconds < s.retryAt) return;
    bool timed = false;
    for (const VideoFilter &f : s.filters) timed |= filterIsTimed(f);
    for (const Source::Step &st : s.steps) timed |= st.part > 0;  // a network run under way goes on
    if (s.filtered && !timed && s.filteredGeneration == s.generation) return;
    const Image *cur = &s.base();
    bool fresh = s.generation != s.filteredGeneration || !s.filtered;  // the step's input is new
    // Until the chain is through: a step that fails (no memory for its
    // picture) leaves the source unfiltered rather than half drawn.
    s.filtered = false;
    // Said at the end if it gets through. A step with a better reason says so.
    s.failure = chainFailsClosed(s.filters) ? FilterFailure::Memory : FilterFailure::None;
    s.retryAt = seconds + 1;
    // Its GPU time, when it all goes into one command buffer.
    uint64_t timedBuffer = 0;
    uint32_t timedFirst = 0;
    if (m_tickNs > 0 && begin() && m_timestampsUsed + 2 <= kTimestamps) {
        timedFirst = m_timestampsUsed;
        m_timestampsUsed += 2;
        timedBuffer = m_bufferSerial;
        vkCmdWriteTimestamp(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_timestamps, timedFirst);
    }
    for (size_t i = 0; i < s.filters.size(); ++i) {
        const VideoFilter &f = s.filters[i];
        Source::Step &st = s.steps[i];
        if (!st.started) { st.started = true; st.origin = seconds; }
        if (filterIsDelay(f)) {
            const double due = seconds - f.delayMs / 1000.0;
            auto retire = [&] { st.spare.push_back(st.held.front().first); st.held.pop_front(); };
            if (f.type == VideoFilterType::RenderDelay || fresh || st.held.empty()) {
                while (st.held.size() >= 2 && st.held[1].second <= due) retire();
                while (st.held.size() >= filterDelayCapacity(cur->width, cur->height)) retire();
                Image img;
                if (!st.spare.empty()) { img = st.spare.back(); st.spare.pop_back(); }
                if (!filterImage(img, cur->width, cur->height)) { destroyImage(img); return; }
                recordFilter(*cur, img, nullptr, FilterParams{});
                st.held.emplace_back(img, seconds);
            }
            while (st.held.size() >= 2 && st.held[1].second <= due) retire();
            if (st.spare.size() > 2) {
                flush();
                while (st.spare.size() > 2) { destroyImage(st.spare.back()); st.spare.pop_back(); }
            }
            cur = &st.held.front().first;
            fresh = true;
            continue;
        }
        int ow, oh;
        filterSize(f, cur->width, cur->height, &ow, &oh);
        if (!filterImage(st.out, ow, oh)) return;
        if (f.type == VideoFilterType::Scale) {
            if (!begin()) return;
            clearTo(st.out, {0, 0, 0, 0});
            drawLayer(st.out, *cur, scaleLayer(f, cur->width, cur->height, ow, oh));
        } else if (f.type == VideoFilterType::Blur || f.type == VideoFilterType::Shadow) {
            const bool blur = f.type == VideoFilterType::Blur;
            if (!filterImage(st.mid, cur->width, cur->height)) return;
            recordFilter(*cur, st.mid, nullptr, filterParams(f, cur->width, cur->height, 0, 0));
            recordFilter(blur ? st.mid : *cur, st.out, blur ? nullptr : &st.mid, filterParams(f, cur->width, cur->height, 0, 1));
        } else if (f.type == VideoFilterType::RemoveBackground) {
            const FilterParams q = filterParams(f, cur->width, cur->height, 0);
            if (!recordMatte(*cur, st, q, f.spread, fresh, s.failure) || !recordGuided(*cur, st, f, q)) return;
        } else {
            const FilterParams q = filterParams(f, cur->width, cur->height, seconds - st.origin, 0, m_levels.sdrWhite);
            const void *auxOf = q.kind == 6 ? static_cast<const void *>(f.lut.get()) : q.kind == 7 ? static_cast<const void *>(f.image.get()) : nullptr;
            if (auxOf && st.auxOf != auxOf) {
                bool ok;
                if (q.kind == 6) {
                    const LookupTable &t = *f.lut;
                    const int texels = t.cube ? t.size * t.size * t.size : t.size;
                    std::vector<float> rgba(size_t(texels) * 4, 1.f);
                    for (int n = 0; n < texels; ++n) std::copy_n(&t.rgb[size_t(n) * 3], 3, &rgba[size_t(n) * 4]);
                    ok = uploadAux(st.aux, VK_FORMAT_R32G32B32A32_SFLOAT, t.size, t.cube ? t.size * t.size : 1, rgba.data(), rgba.size() * sizeof(float));
                } else {
                    ok = uploadAux(st.aux, VK_FORMAT_R8G8B8A8_UNORM, f.image->width, f.image->height, f.image->rgba.data(), f.image->rgba.size());
                }
                if (!ok) { destroyImage(st.aux); st.auxOf = nullptr; return; }
                st.auxOf = auxOf;
            }
            recordFilter(*cur, st.out, auxOf ? &st.aux : nullptr, q);
        }
        cur = &st.out;
        fresh |= filterIsTimed(f);
    }
    if (timedBuffer && timedBuffer == m_bufferSerial && m_recording) {
        vkCmdWriteTimestamp(m_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timestamps, timedFirst + 1);
        m_timings.push_back({id, timedFirst});
    }
    s.filtered = true;
    s.filteredGeneration = s.generation;
    s.failure = FilterFailure::None;
}

bool VkEngine::readPreview(int w, int h, Bgra8Frame &out) { return previewOf(m_canvas, w, h, out); }
bool VkEngine::readSourcePreview(int source, int w, int h, Bgra8Frame &out) {
    const auto it = m_sources.find(source);
    return it != m_sources.end() && previewOf(shownOf(it->second), w, h, out);
}

bool VkEngine::readSourceInput(int source, int w, int h, Bgra8Frame &out) {
    const auto it = m_sources.find(source);
    return it != m_sources.end() && it->second.hasFrame && previewOf(it->second.base(), w, h, out);
}

bool VkEngine::recordPreview(const Image &picture, int w, int h, Buffer &device, Buffer &host) {
    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;
    if (device.size < size || host.size < size) flush();
    if (!ensureBuffer(device, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false) ||
        !ensureBuffer(host, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
        return false;
    if (!begin()) return false;
    const VkDescriptorSet set = descriptorSet(m_preview, picture.view, VK_NULL_HANDLE, device.buffer, size);
    if (!set) return false;
    const PreviewPush push{w, h};
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_preview.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_preview.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, m_preview.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(w, 16), groups(h, 16), 1);
    memoryBarrier();
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(m_cmd, device.buffer, host.buffer, 1, &copy);
    hostReadBarrier();
    return true;
}

bool VkEngine::previewOf(const Image &picture, int w, int h, Bgra8Frame &out) {
    if (!picture.image || !picture.initialised || w <= 0 || h <= 0 || m_lost) return false;
    if (!recordPreview(picture, w, h, m_previewDevice, m_previewHost)) return false;
    flush();
    if (m_lost) return false;
    if (out.width != w || out.height != h) out.resize(w, h);
    std::memcpy(out.data.data(), m_previewHost.mapped, static_cast<size_t>(w) * h * 4);
    return true;
}

bool VkEngine::readOutput(const OutputColour &colour, YuvFrame &out, int width, int height) {
    return outputOf(m_canvas, colour, out, width, height);
}
bool VkEngine::readSourceOutput(int source, const OutputColour &colour, YuvFrame &out, int width, int height) {
    const auto it = m_sources.find(source);
    return it != m_sources.end() && it->second.hasFrame && outputOf(shownOf(it->second), colour, out, width, height);
}

bool VkEngine::recordOutput(const Image &picture, const OutputColour &colour, const YuvFrame &shape, Buffer &device,
                            Buffer &host, Planes *planes, bool toHost) {
    const int w = shape.width, h = shape.height;
    // Each 8x2 block writes whole uints: rows padded to the block's samples.
    const uint32_t bytes = static_cast<uint32_t>(shape.sampleBytes());
    const uint32_t blockW = align(static_cast<uint32_t>(w), 8);
    const bool rgb = formatIsRgb(colour.format);
    const uint32_t chromaSamples = chromaHalfWidth(colour.format) ? blockW / 2 : blockW;
    const uint32_t yStride = rgb ? blockW * 4 : blockW * bytes;
    const uint32_t cStride = rgb ? 0 : chromaSamples * bytes * (chromaInterleaved(colour.format) ? 2 : 1);
    const uint32_t cRows = static_cast<uint32_t>(shape.rows(1)) + 1;  // an odd height's last block row
    const uint32_t uOffset = yStride * align(static_cast<uint32_t>(h), 2);
    const uint32_t vOffset = uOffset + cStride * cRows;
    const VkDeviceSize size = rgb ? uOffset : shape.rowBytes(2) ? vOffset + cStride * cRows : vOffset;
    *planes = Planes{{0, uOffset, vOffset}, {yStride, cStride, cStride}, size};

    if (device.size < size || (toHost && host.size < size)) flush();
    if (!ensureBuffer(device, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false) ||
        (toHost && !ensureBuffer(host, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)))
        return false;
    if (!begin()) return false;
    const VkDescriptorSet set = descriptorSet(m_yuv, picture.view, VK_NULL_HANDLE, device.buffer, size);
    if (!set) return false;
    const YuvOutPush push{w, h, static_cast<int32_t>(yStride), static_cast<int32_t>(cStride),
           static_cast<int32_t>(uOffset), static_cast<int32_t>(vOffset), picture.width, picture.height, outputParams(colour)};
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_yuv.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_yuv.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, m_yuv.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(static_cast<uint32_t>(w), 64), groups(static_cast<uint32_t>(h), 16), 1);
    memoryBarrier();
    if (!toHost) return true;
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(m_cmd, device.buffer, host.buffer, 1, &copy);
    hostReadBarrier();
    return true;
}

void VkEngine::copyOutput(const Buffer &host, const Planes &planes, YuvFrame &out) {
    const uint8_t *src = static_cast<const uint8_t *>(host.mapped);
    std::vector<uint8_t> *dst[3] = {&out.y, &out.u, &out.v};
    for (int p = 0; p < 3; ++p) {
        const int rowBytes = out.rowBytes(p);
        for (int y = 0; y < out.rows(p); ++y)
            std::memcpy(dst[p]->data() + static_cast<size_t>(y) * rowBytes, src + planes.offsets[p] + y * planes.strides[p],
                        rowBytes);
    }
}

bool VkEngine::outputOf(const Image &picture, const OutputColour &colour, YuvFrame &out, int width, int height) {
    if (!picture.image || !picture.initialised || m_lost) return false;
    const int w = width > 0 ? width : picture.width, h = height > 0 ? height : picture.height;
    if (out.width != w || out.height != h || out.format != colour.format) out.resize(w, h, colour.format);
    Planes planes;
    if (!recordOutput(picture, colour, out, m_yuvDevice, m_yuvHost, &planes)) return false;
    flush();
    if (m_lost) return false;
    copyOutput(m_yuvHost, planes, out);
    return true;
}

// Each request keeps its buffers until taken; a free slot's buffers are in no
// recorded or submitted work, so they may be resized at will.
VkEngine::Readback &VkEngine::freeReadback() {
    for (Readback &r : m_readbacks)
        if (r.id == 0) return r;
    return m_readbacks.emplace_back();
}

int VkEngine::requestOutput(const OutputColour &colour, int width, int height) {
    if (!m_canvas.image || !m_canvas.initialised || m_lost) return 0;
    Readback &r = freeReadback();
    r.shape = YuvFrame{};
    r.shape.width = width > 0 ? width : m_canvas.width;
    r.shape.height = height > 0 ? height : m_canvas.height;
    r.shape.format = colour.format;
    r.shape.nv12 = colour.format == ColourFormat::NV12;
    if (!recordOutput(m_canvas, colour, r.shape, r.device, r.host, &r.planes)) return 0;
    r.preview = false;
    r.id = m_nextRequest++;
    return r.id;
}

int VkEngine::requestPreview(int width, int height) {
    if (!m_canvas.image || !m_canvas.initialised || width <= 0 || height <= 0 || m_lost) return 0;
    Readback &r = freeReadback();
    if (!recordPreview(m_canvas, width, height, r.device, r.host)) return 0;
    r.shape = YuvFrame{};
    r.shape.width = width;
    r.shape.height = height;
    r.preview = true;
    r.id = m_nextRequest++;
    return r.id;
}

bool VkEngine::takeOutput(int request, YuvFrame &out) {
    if (m_recording) flush();  // asked for and never submitted
    settle();
    for (Readback &r : m_readbacks) {
        if (r.id != request || r.preview) continue;
        r.id = 0;
        if (m_lost) return false;
        if (out.width != r.shape.width || out.height != r.shape.height || out.format != r.shape.format)
            out.resize(r.shape.width, r.shape.height, r.shape.format);
        copyOutput(r.host, r.planes, out);
        return true;
    }
    return false;
}

// What FFmpeg calls each of our colour formats.
static AVPixelFormat pixelFormatFor(ColourFormat format) {
    switch (format) {
    case ColourFormat::NV12: return AV_PIX_FMT_NV12;
    case ColourFormat::I420: return AV_PIX_FMT_YUV420P;
    case ColourFormat::I444: return AV_PIX_FMT_YUV444P;
    case ColourFormat::P010: return AV_PIX_FMT_P010LE;
    case ColourFormat::I010: return AV_PIX_FMT_YUV420P10LE;
    case ColourFormat::P216: return AV_PIX_FMT_P216LE;
    case ColourFormat::P416: return AV_PIX_FMT_P416LE;
    case ColourFormat::BGRA: return AV_PIX_FMT_BGRA;
    case ColourFormat::I210: return AV_PIX_FMT_YUV422P10LE;
    }
    return AV_PIX_FMT_NONE;
}

bool VkEngine::writeVulkanFrames(const OutputColour &colour, int width, int height, const std::vector<AVFrame *> &frames) {
    if (!m_ffDevice || frames.empty() || !m_canvas.image || !m_canvas.initialised || m_lost) return false;
    const int w = width > 0 ? width : m_canvas.width, h = height > 0 ? height : m_canvas.height;
    YuvFrame shape;
    shape.width = w;
    shape.height = h;
    shape.format = colour.format;
    shape.nv12 = colour.format == ColourFormat::NV12;
    const int planes = shape.rowBytes(2) ? 3 : shape.rowBytes(1) ? 2 : 1;
    // Every frame on this device, in this format and size, or none is written.
    for (const AVFrame *frame : frames) {
        if (!frame || frame->format != AV_PIX_FMT_VULKAN || !frame->hw_frames_ctx) return false;
        const auto *fc = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data);
        if (fc->device_ref->data != m_ffDevice->data || fc->sw_format != pixelFormatFor(colour.format) ||
            fc->width != w || fc->height != h || av_pix_fmt_count_planes(fc->sw_format) != planes)
            return false;
    }

    // One conversion into a buffer of the GPU's own, copied into each frame.
    Readback &r = freeReadback();
    r.shape = shape;
    if (!recordOutput(m_canvas, colour, r.shape, r.device, r.host, &r.planes, false)) return false;
    r.preview = false;
    r.id = -1;
    for (AVFrame *frame : frames) {
        auto *fc = reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data);
        auto *vkfc = static_cast<AVVulkanFramesContext *>(fc->hwctx);
        auto *vkf = reinterpret_cast<AVVkFrame *>(frame->data[0]);
        int images = 0;
        while (images < AV_NUM_DATA_POINTERS && vkf->img[images]) ++images;
        const bool multiplane = images == 1 && planes > 1;
        vkfc->lock_frame(fc, vkf);
        // From wherever the encoder left it; it waits on the semaphore below
        // before it reads it again.
        for (int i = 0; i < images; ++i) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = vkf->layout[i];
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = vkf->img[i];
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &b);
        }
        for (int p = 0; p < planes; ++p) {
            const int texels = p ? shape.chromaWidth() : w;
            VkBufferImageCopy region{};
            region.bufferOffset = r.planes.offsets[p];
            region.bufferRowLength = r.planes.strides[p] / static_cast<uint32_t>(shape.rowBytes(p) / texels);
            region.imageSubresource.aspectMask =
                multiplane ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_PLANE_0_BIT << p) : VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {static_cast<uint32_t>(texels), static_cast<uint32_t>(p ? shape.chromaHeight() : h), 1};
            vkCmdCopyBufferToImage(m_cmd, r.device.buffer, vkf->img[multiplane ? 0 : p],
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        for (int i = 0; i < images; ++i) {
            vkf->layout[i] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            vkf->access[i] = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            m_waitSems.push_back(vkf->sem[i]);
            m_waitValues.push_back(vkf->sem_value[i]);
            m_waitStages.push_back(VK_PIPELINE_STAGE_TRANSFER_BIT);
            m_signalSems.push_back(vkf->sem[i]);
            m_signalValues.push_back(++vkf->sem_value[i]);
        }
        vkfc->unlock_frame(fc, vkf);
    }
    return true;
}

bool VkEngine::takePreview(int request, Bgra8Frame &out) {
    if (m_recording) flush();
    settle();
    for (Readback &r : m_readbacks) {
        if (r.id != request || !r.preview) continue;
        r.id = 0;
        if (m_lost) return false;
        if (out.width != r.shape.width || out.height != r.shape.height) out.resize(r.shape.width, r.shape.height);
        std::memcpy(out.data.data(), r.host.mapped, static_cast<size_t>(r.shape.width) * r.shape.height * 4);
        return true;
    }
    return false;
}

}  // namespace

std::vector<GpuDevice> listGpus() { return enumerateGpus(); }

GpuPick chooseGpu(const std::vector<GpuDevice> &gpus, const std::string &chosen, bool onBattery) {
    GpuPick pick;
    if (!chosen.empty()) {
        for (const GpuDevice &g : gpus)
            if (g.usable && g.id == chosen) return {g.index, GpuPick::Why::Chosen};
    }
    const auto rank = [onBattery](GpuDevice::Kind kind) {
        switch (kind) {
        case GpuDevice::Kind::Discrete: return onBattery ? 2 : 3;
        case GpuDevice::Kind::Integrated: return onBattery ? 3 : 2;
        case GpuDevice::Kind::Virtual: return 1;
        default: return 0;  // the CPU twin beats a software Vulkan device
        }
    };
    int best = 0;
    const GpuDevice *picked = nullptr;
    for (const GpuDevice &g : gpus) {
        if (!g.usable) continue;
        if (const int r = rank(g.kind); r > best) { best = r; picked = &g; }
    }
    if (!picked) return pick;
    pick = {picked->index, GpuPick::Why::Automatic};
    // Only say "on battery" when the battery changed the answer.
    if (onBattery && picked->kind == GpuDevice::Kind::Integrated &&
        std::any_of(gpus.begin(), gpus.end(), [](const GpuDevice &g) { return g.usable && g.kind == GpuDevice::Kind::Discrete; }))
        pick.why = GpuPick::Why::OnBattery;
    if (!chosen.empty()) pick.why = GpuPick::Why::ChosenMissing;
    return pick;
}

std::recursive_mutex &loaderMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

std::unique_ptr<Engine> createVulkanEngine(int deviceIndex, std::string *failure, AVBufferRef *shared) {
    auto e = std::make_unique<VkEngine>();
    if (failure) failure->clear();
    if (!e->init(deviceIndex, failure, shared)) {
        if (failure && failure->empty()) *failure = "the GPU engine could not start";
        return nullptr;
    }
    return e;
}

}  // namespace engine
