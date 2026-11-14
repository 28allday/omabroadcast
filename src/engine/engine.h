#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "frame.h"

struct AVBufferRef;
struct AVFrame;

namespace engine {

enum class Mode { Auto, Vulkan, Cpu };

// A Vulkan device as the machine reports it. The render node is how other
// APIs (VA-API) name the same card; empty when the driver does not say.
struct GpuDevice {
    enum class Kind { Discrete, Integrated, Virtual, Other };
    int index = 0;
    std::string id;          // the driver's device UUID: the same card from start to start
    std::string name;
    std::string renderNode;  // e.g. /dev/dri/renderD128
    Kind kind = Kind::Other;
    bool hardware = false;   // a real GPU, not a software rasteriser
    bool usable = false;     // new enough (Vulkan 1.2) for the engine
    uint32_t vendorId = 0;   // PCI vendor, as the driver reports it
};
std::vector<GpuDevice> listGpus();

// Which GPU composites. The one chosen (by id) when it is here; otherwise a
// discrete GPU, or an integrated one on battery, where waking the discrete
// one costs the most power. Ties go to the order the driver lists them.
struct GpuPick {
    enum class Why { Chosen, Automatic, OnBattery, ChosenMissing, None };
    int index = -1;  // into the list; -1 when no GPU can composite
    Why why = Why::None;
};
GpuPick chooseGpu(const std::vector<GpuDevice> &gpus, const std::string &chosen, bool onBattery);

// Hold while creating or destroying a Vulkan instance or device, or an FFmpeg
// hardware device (which may make its own Vulkan instance). Entering the
// Vulkan loader from two threads at once here has crashed it (null dispatch
// in vkEnumerateInstanceExtensionProperties while another thread enumerated
// devices), so setup and teardown take turns; everyday calls do not need it.
std::recursive_mutex &loaderMutex();

// The compositor. One implementation runs on Vulkan, the other on the CPU;
// both honour exactly this contract.
//
// Not thread-safe: one owner thread drives it.
// Why a source that must be cut out is not being drawn.
enum class FilterFailure {
    None,
    Model,   // the network's weights did not load
    Device,  // the graphics card cannot hold the network
    Memory   // no memory for a picture or the network's working space
};

class Engine {
public:
    virtual ~Engine() = default;

    // Tries Vulkan unless told otherwise, falls back to the CPU twin. Never null.
    static std::unique_ptr<Engine> create(Mode mode = Mode::Auto, int deviceIndex = -1);

    virtual bool isGpu() const = 0;
    virtual std::string deviceName() const = 0;
    // The DRM render node of the device compositing; empty on the CPU twin.
    virtual std::string renderNode() const { return {}; }
    // Why the GPU was not used (empty when it was, or when none was asked
    // for): what createVulkanEngine said, handed to createCpuEngine.
    virtual std::string note() const { return {}; }

    // Sources ------------------------------------------------------------
    virtual int createSource() = 0;
    virtual void destroySource(int id) = 0;
    // Both return false when the frame could not be taken; the previous
    // contents of the source stay as they were.
    virtual bool updateSource(int id, const CpuFrame &frame) = 0;
    virtual bool updateSource(int id, const DmaBufFrame &frame) = 0;
    virtual bool updateSource(int id, const YuvInput &frame) = 0;
    // Noise reduction for a source's new frames, 0 (off) to 1 (strongest):
    // spatial chroma smoothing guided by brightness, then a motion-adaptive
    // average with its own recent past. Costs nothing when off.
    virtual void setSourceDenoise(int id, float strength) = 0;
    // Configure before upload; render the due field after upload and before
    // compositing. Kept rows are even (0) or odd (1). firstInFrame identifies
    // the earlier field for the blend mode's temporal neighbour.
    virtual void setSourceDeinterlace(int id, DeinterlaceMode mode) = 0;
    virtual void renderSourceField(int id, int field, bool firstInFrame) = 0;
    virtual void resetSourceHistory(int id) = 0;
    // A source's video filters (see VideoFilter in frame.h), in order. Its
    // picture, as uploaded, fielded and cleaned or drawn, is what they take;
    // what they make is what the source shows from renderSourceFilters on.
    // Setting the same chain again keeps its delays and scroll positions.
    virtual void setSourceFilters(int id, const std::vector<VideoFilter> &filters) = 0;
    // Runs the chain once per frame, after renderSourceField or compositeInto
    // and before the composite that shows the source. `seconds` is the
    // caller's clock: scroll moves and delays count on it; the engine reads no
    // clock of its own. An unchanged, untimed chain is not run again.
    virtual void renderSourceFilters(int id, double seconds) = 0;
    // A chain that cannot finish leaves the source unfiltered, with one exception.
    // Remove background is there to keep something off air, so a chain holding it
    // that cannot finish draws nothing at all, and this says why. It is tried again
    // about once a second.
    // The same goes for a chain that is there to draw nothing (a Colour step with
    // no opacity): see chainFailsClosed in frame.h.
    virtual FilterFailure sourceFilterFailure(int id) const { (void)id; return FilterFailure::None; }
    // A hidden source draws nothing wherever it is shown (composites,
    // transitions, readSourcePreview, readSourceOutput), whatever its chain does:
    // it takes no picture and no memory, so it cannot fail. Its frames are still
    // taken and its chain still runs, so it is up to date the moment it is shown
    // again; give it an empty chain as well to save that work. readSourceInput
    // still reads its own picture. Sources start shown.
    virtual void setSourceHidden(int id, bool hidden) = 0;
    virtual bool sourceHidden(int id) const = 0;
    // What drawing a source's filters costs per frame they run, in ms
    // (GPU time on Vulkan, smoothed over the last few); -1 until measured.
    virtual double filterMs(int id) const { (void)id; return -1; }
    // What the canvas's 1.0 means in nits and the HDR nominal peak: how PQ and
    // HLG sources are read in, and what the tone-map and SDR-on-HDR filters
    // work against. Takes effect from each source's next frame.
    virtual void setHdrLevels(const HdrLevels &levels) = 0;
    virtual HdrLevels hdrLevels() const = 0;
    // Modifiers the engine can import for a DRM fourcc, for capture negotiation.
    // Empty on the CPU twin, which only takes memory frames.
    virtual std::vector<uint64_t> importModifiers(uint32_t fourcc) const { (void)fourcc; return {}; }

    // Canvas -------------------------------------------------------------
    virtual void setCanvasSize(int width, int height) = 0;
    virtual int canvasWidth() const = 0;
    virtual int canvasHeight() const = 0;

    // Clears to `background` then draws the layers bottom to top.
    virtual void composite(const std::vector<Layer> &layers, const Color &background) = 0;
    // The same, into a source's own picture (width x height) rather than the
    // canvas: how a studio shows inside another, and how a group is drawn.
    // A layer showing the target itself is skipped. Do these before the
    // composite that uses them.
    virtual void compositeInto(int target, int width, int height, const std::vector<Layer> &layers,
                               const Color &background) = 0;

    // Mix two full pictures into a distinct source (see TransitionStyle in
    // frame.h). Progress is clamped to 0..1.
    virtual void transitionInto(int target, int width, int height, int from, int to,
                                const TransitionStyle &style, float progress) = 0;
    // The slideshow's four, with their original sides: swipe reveals from
    // the left, slide pushes the old picture to the left.
    void transitionInto(int target, int width, int height, int from, int to, Transition type, float progress) {
        TransitionStyle style;
        style.type = type;
        style.side = type == Transition::Slide ? Side::Right : Side::Left;
        transitionInto(target, width, height, from, to, style, progress);
    }

    // Readbacks of the last composite. The preview is bilinear-downscaled.
    virtual bool readPreview(int width, int height, Bgra8Frame &out) = 0;
    // Full source snapshot, independent of item crops and the production canvas.
    virtual bool readSourcePreview(int source, int width, int height, Bgra8Frame &out) = 0;
    // The same, of the source's own picture before its filters (a key's backdrop is sampled there).
    virtual bool readSourceInput(int source, int width, int height, Bgra8Frame &out) = 0;
    // The canvas in an output's format and colour (see OutputColour and
    // OutputParams in frame.h), at its own size or rescaled to width ×
    // height: each output pixel averages ceil(scale)² bilinear samples in
    // linear light (plain bilinear when enlarging); subsampled chroma is the
    // mean of the encoded R'G'B' it covers.
    virtual bool readOutput(const OutputColour &colour, YuvFrame &out, int width, int height) = 0;
    // The same of a source's own picture (as last drawn or uploaded) rather
    // than the canvas; 0 keeps its size.
    virtual bool readSourceOutput(int source, const OutputColour &colour, YuvFrame &out, int width, int height) = 0;
    // 8-bit BT.709 limited-range 4:2:0, NV12 or I420: what the virtual camera
    // and every default output take.
    static OutputColour sdr420(bool nv12) { OutputColour c; c.format = nv12 ? ColourFormat::NV12 : ColourFormat::I420; return c; }
    bool readYuv(bool nv12, YuvFrame &out, int width, int height) { return readOutput(sdr420(nv12), out, width, height); }
    bool readYuv(bool nv12, YuvFrame &out) { return readOutput(sdr420(nv12), out, 0, 0); }
    bool readSourceYuv(int source, bool nv12, YuvFrame &out, int width, int height) {
        return readSourceOutput(source, sdr420(nv12), out, width, height);
    }

    // Finishes every recorded operation. After it returns, frames handed to
    // updateSource may be released by their owner. The readbacks flush too.
    virtual void flush() {}

    // Readbacks a tick later, so the frame loop never waits for the GPU:
    // requestOutput and requestPreview ask for what readOutput and
    // readPreview give and return a number (0 when they cannot); submit()
    // sends the work recorded so far and returns at once; collect() waits for
    // it, after which takeOutput and takePreview hand each request over, once.
    // Frames handed to updateSource may be released after collect(). Any other
    // operation first waits for what was submitted, so mixing the two is safe.
    // The CPU twin reads at the request; the result is the same either way.
    virtual int requestOutput(const OutputColour &colour, int width, int height) {
        YuvFrame frame;
        if (!m_spareOutputs.empty()) { frame = std::move(m_spareOutputs.back()); m_spareOutputs.pop_back(); }
        if (!readOutput(colour, frame, width, height)) return 0;
        const int id = m_nextRequest++;
        m_doneOutputs.push_back({id, std::move(frame)});
        return id;
    }
    virtual int requestPreview(int width, int height) {
        Bgra8Frame frame;
        if (!readPreview(width, height, frame)) return 0;
        const int id = m_nextRequest++;
        m_donePreviews.push_back({id, std::move(frame)});
        return id;
    }
    virtual void submit() { flush(); }
    virtual void collect() {}
    // The caller's picture is swapped in, so its buffer is used again.
    virtual bool takeOutput(int request, YuvFrame &out) {
        for (auto it = m_doneOutputs.begin(); it != m_doneOutputs.end(); ++it) {
            if (it->first != request) continue;
            std::swap(out, it->second);
            if (m_spareOutputs.size() < 4 && !it->second.y.empty()) m_spareOutputs.push_back(std::move(it->second));
            m_doneOutputs.erase(it);
            return true;
        }
        return false;
    }
    virtual bool takePreview(int request, Bgra8Frame &out) {
        for (auto it = m_donePreviews.begin(); it != m_donePreviews.end(); ++it) {
            if (it->first != request) continue;
            std::swap(out, it->second);
            m_donePreviews.erase(it);
            return true;
        }
        return false;
    }

    // True once the device is gone; the owner should recreate on the CPU.
    // Destroying a lost engine never waits on its device without limit: one
    // that stopped answering gets a second at most, and what it was given is
    // then left with it rather than freed under it.
    virtual bool lost() const { return false; }

    // On a Vulkan device of FFmpeg's (see createVulkanEngine), the device
    // context (AVHWDeviceContext data) its encoders can share; else null.
    virtual const void *sharedDevice() const { return nullptr; }
    // The canvas in `colour`, converted once and copied into Vulkan frames of
    // encoders on that device: nothing is read back. Each is ready once the
    // work is submitted (its semaphore says when), so it may go to its encoder
    // straight away. False, and nothing written, unless every frame is on this
    // device in `colour`'s format and this size.
    virtual bool writeVulkanFrames(const OutputColour &colour, int width, int height, const std::vector<AVFrame *> &frames) {
        (void)colour; (void)width; (void)height; (void)frames;
        return false;
    }

protected:
    int m_nextRequest = 1;

private:
    std::vector<std::pair<int, YuvFrame>> m_doneOutputs;
    std::vector<YuvFrame> m_spareOutputs;
    std::vector<std::pair<int, Bgra8Frame>> m_donePreviews;
};

// `note`: why it stands in for a GPU (see Engine::note), when it does.
std::unique_ptr<Engine> createCpuEngine(const std::string &note = {});
// `failure` always says why when the answer is null.
// With `shared`, an FFmpeg Vulkan device (AVHWDeviceContext) on that GPU:
// the engine works on it rather than a device of its own, so encoders on it
// take their pictures without leaving the GPU.
std::unique_ptr<Engine> createVulkanEngine(int deviceIndex, std::string *failure, AVBufferRef *shared = nullptr);

}  // namespace engine
