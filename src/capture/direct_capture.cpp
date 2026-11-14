#include "direct_capture.h"

#include <wayland-client.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include "linux-dmabuf-v1-client.h"
#include "hyprland-toplevel-export-v1-client.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client.h"
#include "wlr-screencopy-unstable-v1-client.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <thread>
#include <utility>
#include <random>
#include <cerrno>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct GpuDevice {
    int fd = -1;
    gbm_device *device = nullptr;
    std::mutex mutex;
    explicit GpuDevice(const std::string &path) {
        fd=open(path.c_str(),O_RDWR|O_CLOEXEC);
        if(fd>=0) device=gbm_create_device(fd);
    }
    ~GpuDevice() { if(device) gbm_device_destroy(device); if(fd>=0) close(fd); }
};
struct GpuMemory {
    std::shared_ptr<GpuDevice> device;
    gbm_bo *bo = nullptr;
    engine::DmaBufFrame frame;
    ~GpuMemory() {
        for(const auto &plane:frame.planes) if(plane.fd>=0) close(plane.fd);
        if(bo) { std::lock_guard lock(device->mutex); gbm_bo_destroy(bo); }
    }
};
}

struct DirectCapture::Stream::Data {
    mutable std::mutex mutex;
    Settings settings;
    uint64_t version = 1, received = 0, dropped = 0, generation = 0;
    State state = State::Waiting;
    std::string error;
    bool stopped = false, picture = false;
    int width = 0, height = 0;
    std::shared_ptr<const Frame> latest;
};
DirectCapture::Stream::Stream(std::shared_ptr<Data> data) : d(std::move(data)) {}
DirectCapture::Stream::~Stream() { stop(); }
void DirectCapture::Stream::configure(const Settings &settings) {
    std::lock_guard lock(d->mutex);
    if (d->settings == settings && !d->stopped) return;
    d->settings = settings; ++d->version; d->stopped = false;
    d->state = State::Waiting; d->error.clear(); d->picture = false; d->latest.reset();
}
void DirectCapture::Stream::setDmaBufDevice(const std::string &node, const std::map<uint32_t,std::vector<uint64_t>> &modifiers) {
    std::lock_guard lock(d->mutex);
    if(d->stopped || (d->settings.renderNode==node && d->settings.modifiers==modifiers)) return;
    d->settings.renderNode=node; d->settings.modifiers=modifiers; ++d->version;
    d->picture=false; d->latest.reset(); d->state=State::Waiting;
}
void DirectCapture::Stream::disableDmaBuf() {
    std::lock_guard lock(d->mutex);
    if(d->stopped || d->settings.renderNode.empty()) return;
    d->settings.renderNode.clear(); d->settings.modifiers.clear(); ++d->version;
    d->picture=false; d->latest.reset(); d->state=State::Waiting;
}
void DirectCapture::Stream::stop() {
    std::lock_guard lock(d->mutex);
    d->stopped = true; d->state = State::Stopped; d->picture = false; d->latest.reset(); ++d->version;
}
std::shared_ptr<const DirectCapture::Frame> DirectCapture::Stream::acquire() {
    std::lock_guard lock(d->mutex); return std::exchange(d->latest, {});
}
DirectCapture::State DirectCapture::Stream::state() const { std::lock_guard lock(d->mutex); return d->state; }
std::string DirectCapture::Stream::error() const { std::lock_guard lock(d->mutex); return d->error; }
bool DirectCapture::Stream::hasPicture() const { std::lock_guard lock(d->mutex); return d->picture; }
int DirectCapture::Stream::width() const { std::lock_guard lock(d->mutex); return d->width; }
int DirectCapture::Stream::height() const { std::lock_guard lock(d->mutex); return d->height; }
uint64_t DirectCapture::Stream::received() const { std::lock_guard lock(d->mutex); return d->received; }
uint64_t DirectCapture::Stream::dropped() const { std::lock_guard lock(d->mutex); return d->dropped; }
uint64_t DirectCapture::Stream::generation() const { std::lock_guard lock(d->mutex); return d->generation; }

struct DirectCapture::Impl {
    struct Output { Impl *owner; uint32_t global; wl_output *object; std::string name, description; engine::BufferTransform transform=engine::BufferTransform::Normal, pendingTransform=engine::BufferTransform::Normal; };
    struct Window {
        Impl *owner; zwlr_foreign_toplevel_handle_v1 *object;
        std::string key, title, appId; bool focused = false, closed = false;
    };
    struct Capture {
        Impl *owner;
        std::shared_ptr<Stream::Data> data;
        Settings settings;
        uint64_t version = 0, generation = 0;
        wl_output *output = nullptr;
        zwlr_foreign_toplevel_handle_v1 *window = nullptr;
        zwlr_screencopy_frame_v1 *screenFrame = nullptr;
        hyprland_toplevel_export_frame_v1 *windowFrame = nullptr;
        wl_buffer *buffer = nullptr;
        void *mapping = MAP_FAILED;
        size_t bytes = 0;
        uint32_t format = 0, width = 0, height = 0, stride = 0, flags = 0;
        int64_t requested = 0, next = 0;
        bool copied = false, dmaDisabled = false;
        int dmaFailures = 0;  // in a row: a few go before the GPU path is given up
        engine::BufferTransform transform=engine::BufferTransform::Normal;
        uint32_t dmaFormat = 0, dmaWidth = 0, dmaHeight = 0;
        struct GpuBuffer {
            wl_buffer *buffer = nullptr;
            std::shared_ptr<GpuMemory> memory;
            std::weak_ptr<const void> lease;
            ~GpuBuffer() { if(buffer) wl_buffer_destroy(buffer); }
        };
        std::shared_ptr<GpuDevice> gpu;
        std::vector<std::unique_ptr<GpuBuffer>> pool;
        GpuBuffer *active = nullptr;
        zwp_linux_buffer_params_v1 *params = nullptr;
        ~Capture() { clear(); }
        void resetPool() { pool.clear(); gpu.reset(); dmaDisabled=false; }
        void offeredDma(uint32_t f,uint32_t w,uint32_t h) { dmaFormat=f; dmaWidth=w; dmaHeight=h; }
        void sendCopy(wl_buffer *target) {
            copied=true;
            if(screenFrame) zwlr_screencopy_frame_v1_copy(screenFrame,target);
            else hyprland_toplevel_export_frame_v1_copy(windowFrame,target,1);
        }
        bool copyDma() {
            if(dmaDisabled || !owner->dmabuf || settings.renderNode.empty() || !dmaWidth || !dmaHeight ||
                dmaWidth>16384 || dmaHeight>16384 || uint64_t(dmaWidth)*dmaHeight*4>256*1024*1024) return false;
            auto available=settings.modifiers.find(dmaFormat);
            if(available==settings.modifiers.end()) return false;
            std::vector<uint64_t> mods;
            for(auto modifier:available->second)
                if(modifier!=DRM_FORMAT_MOD_INVALID && std::find(owner->dmaModifiers[dmaFormat].begin(),owner->dmaModifiers[dmaFormat].end(),modifier)!=owner->dmaModifiers[dmaFormat].end()) mods.push_back(modifier);
            if(mods.empty()) return false;
            // Only unleased buffers may be rewritten. Resizing retires old
            // protocol objects; retained frames and engine imports own memory.
            std::erase_if(pool,[&](const auto &b) { return b->memory->frame.width!=int(dmaWidth) || b->memory->frame.height!=int(dmaHeight) || b->memory->frame.fourcc!=dmaFormat; });
            for(auto &b:pool) if(b->lease.expired()) { active=b.get(); sendCopy(active->buffer); return true; }
            if(pool.size()>=3) {
                // A slow consumer may hold all three pictures. Skip this tick,
                // retaining the last displayed picture and a bounded pool.
                clear(); return true;
            }
            if(!gpu) gpu=std::make_shared<GpuDevice>(settings.renderNode);
            if(!gpu->device) { dmaDisabled=true; return false; }
            auto memory=std::make_shared<GpuMemory>(); memory->device=gpu;
            {
                std::lock_guard lock(gpu->mutex);
                memory->bo=gbm_bo_create_with_modifiers2(gpu->device,dmaWidth,dmaHeight,dmaFormat,mods.data(),mods.size(),GBM_BO_USE_RENDERING);
                if(!memory->bo || gbm_bo_get_plane_count(memory->bo)!=1) { dmaDisabled=true; return false; }
                auto &f=memory->frame;
                f.width=dmaWidth; f.height=dmaHeight; f.fourcc=dmaFormat; f.modifier=gbm_bo_get_modifier(memory->bo);
                if(std::find(mods.begin(),mods.end(),f.modifier)==mods.end()) { dmaDisabled=true; return false; }
                const int fd=gbm_bo_get_fd_for_plane(memory->bo,0);
                if(fd<0) { dmaDisabled=true; return false; }
                f.planes.push_back({fd,gbm_bo_get_offset(memory->bo,0),gbm_bo_get_stride_for_plane(memory->bo,0)});
            }
            auto entry=std::make_unique<GpuBuffer>(); entry->memory=memory; active=entry.get(); pool.push_back(std::move(entry));
            params=zwp_linux_dmabuf_v1_create_params(owner->dmabuf);
            static const zwp_linux_buffer_params_v1_listener listener={
                [](void *p,auto *params,wl_buffer *buffer) {
                    auto &c=*static_cast<Capture *>(p);
                    zwp_linux_buffer_params_v1_destroy(params); c.params=nullptr;
                    c.active->buffer=buffer;
                    static const wl_buffer_listener release={[](void *,wl_buffer *) {}};
                    wl_buffer_add_listener(buffer,&release,nullptr);
                    c.sendCopy(buffer);
                },
                [](void *p,auto *params) {
                    auto &c=*static_cast<Capture *>(p);
                    zwp_linux_buffer_params_v1_destroy(params); c.params=nullptr;
                    c.active=nullptr; c.pool.clear(); c.dmaDisabled=true;
                    c.copy();
                }
            };
            zwp_linux_buffer_params_v1_add_listener(params,&listener,this);
            const auto &f=memory->frame; const auto &plane=f.planes[0];
            zwp_linux_buffer_params_v1_add(params,plane.fd,0,plane.offset,plane.stride,uint32_t(f.modifier>>32),uint32_t(f.modifier));
            zwp_linux_buffer_params_v1_create(params,f.width,f.height,f.fourcc,0);
            return true;
        }
        bool pending() const { return screenFrame || windowFrame; }
        void clear() {
            if(params) zwp_linux_buffer_params_v1_destroy(params);
            params=nullptr; active=nullptr; dmaFormat=dmaWidth=dmaHeight=0;
            if (screenFrame) zwlr_screencopy_frame_v1_destroy(screenFrame);
            if (windowFrame) hyprland_toplevel_export_frame_v1_destroy(windowFrame);
            screenFrame = nullptr; windowFrame = nullptr;
            if (buffer) wl_buffer_destroy(buffer);
            buffer = nullptr;
            if (mapping != MAP_FAILED) munmap(mapping, bytes);
            mapping = MAP_FAILED; bytes = 0; width = height = stride = flags = 0; copied = false;
            std::erase_if(pool,[](const auto &b) { return !b->buffer; });
        }
        void state(State state, const std::string &message = {}) {
            std::lock_guard lock(data->mutex);
            if (data->version != version || data->stopped) return;
            data->state = state; data->error = message;
            if (state != State::Live) { data->picture = false; data->latest.reset(); }
        }
        void fail(const std::string &message) {
            // One failure can be passing (a mode change); three in a row are not.
            if(active && ++dmaFailures >= 3) { dmaDisabled=true; }
            clear(); generation=++owner->generation; state(State::Failed, message); next = nowNs() + 500000000;
        }
        void copy() {
            if (copied || params) return;
            if (copyDma()) return;
            if (!width || !height || width > 16384 || height > 16384 || stride < uint64_t(width) * 4 ||
                uint64_t(stride) * height > 256 * 1024 * 1024 ||
                (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888 &&
                 format != WL_SHM_FORMAT_ABGR8888 && format != WL_SHM_FORMAT_XBGR8888)) {
                fail("The compositor offered an unsupported capture buffer"); return;
            }
            bytes = size_t(stride) * height;
            const int fd = memfd_create("omabroadcast-capture", MFD_CLOEXEC);
            if (fd < 0) { fail("Cannot create a capture buffer"); return; }
            if (ftruncate(fd, off_t(bytes)) < 0) { close(fd); fail("Cannot size a capture buffer"); return; }
            mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (mapping == MAP_FAILED) { close(fd); fail("Cannot map a capture buffer"); return; }
            auto *pool = wl_shm_create_pool(owner->shm, fd, int(bytes)); close(fd);
            buffer = wl_shm_pool_create_buffer(pool, 0, int(width), int(height), int(stride), format);
            wl_shm_pool_destroy(pool);
            sendCopy(buffer);
        }
        void ready() {
            if (!copied || (!active && mapping == MAP_FAILED)) { fail("Capture completed without a buffer"); return; }
            auto frame = std::make_shared<Frame>();
            frame->timeNs = nowNs(); frame->generation = generation;
            if(active) {
                dmaFailures=0;
                frame->dmabuf=true; frame->dma=active->memory->frame;
                frame->dma.owner=active->memory; frame->dma.token=active->memory.get();
                frame->dma.premultiplied=dmaFormat==DRM_FORMAT_ARGB8888 || dmaFormat==DRM_FORMAT_ABGR8888;
                frame->dma.yInvert=flags&1; frame->dma.transform=transform;
                frame->premultiplied=frame->dma.premultiplied;
                frame->format=(dmaFormat==DRM_FORMAT_ABGR8888 || dmaFormat==DRM_FORMAT_XBGR8888) ? engine::PixelFormat::RGBA : engine::PixelFormat::BGRA;
                frame->stride=0;
                frame->width=engine::swapsAxes(transform) ? frame->dma.height : frame->dma.width;
                frame->height=engine::swapsAxes(transform) ? frame->dma.width : frame->dma.height;
                frame->lease=std::make_shared<int>(0); active->lease=frame->lease;
            } else {
                frame->width = int(engine::swapsAxes(transform) ? height : width);
                frame->height = int(engine::swapsAxes(transform) ? width : height);
                frame->stride = frame->width * 4;
                frame->format = (format == WL_SHM_FORMAT_ABGR8888 || format == WL_SHM_FORMAT_XBGR8888) ? engine::PixelFormat::RGBA : engine::PixelFormat::BGRA;
                frame->premultiplied = format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_ABGR8888;
                frame->pixels.resize(size_t(frame->stride) * frame->height);
                for (int y = 0; y < frame->height; ++y) {
                    auto *to = frame->pixels.data() + size_t(y) * frame->stride;
                    if(transform==engine::BufferTransform::Normal) {
                        const int row=flags&1 ? int(height)-1-y : y;
                        memcpy(to,static_cast<const uint8_t *>(mapping)+size_t(row)*stride,frame->stride);
                    } else for(int x=0; x<frame->width; ++x) {
                        int sx,sy; engine::bufferPixel(transform,int(width),int(height),x,y,flags&1,sx,sy);
                        memcpy(to+x*4,static_cast<const uint8_t *>(mapping)+size_t(sy)*stride+sx*4,4);
                    }
                    if (!frame->premultiplied) for (int x = 0; x < frame->width; ++x) to[x * 4 + 3] = 255;
                }
            }
            {
                std::lock_guard lock(data->mutex);
                if (data->version == version && !data->stopped) {
                    if (data->latest) ++data->dropped;
                    data->latest = frame; data->picture = true; ++data->received;
                    data->generation = frame->generation;
                    data->width = frame->width; data->height = frame->height;
                    data->state = State::Live; data->error.clear();
                }
            }
            clear();
        }
        void request() {
            requested = nowNs(); next = requested + 1000000000 / std::clamp(settings.fps, 1, 240);
            if (window) {
                windowFrame = hyprland_toplevel_export_manager_v1_capture_toplevel_with_wlr_toplevel_handle(owner->exporter, settings.cursor, window);
                static const hyprland_toplevel_export_frame_v1_listener listener = {
                    [](void *p, auto *, uint32_t f, uint32_t w, uint32_t h, uint32_t s) { auto &c=*static_cast<Capture *>(p); c.format=f; c.width=w; c.height=h; c.stride=s; },
                    [](void *, auto *, uint32_t, uint32_t, uint32_t, uint32_t) {},
                    [](void *p, auto *, uint32_t f) { static_cast<Capture *>(p)->flags=f; },
                    [](void *p, auto *, uint32_t, uint32_t, uint32_t) { static_cast<Capture *>(p)->ready(); },
                    [](void *p, auto *) { static_cast<Capture *>(p)->fail("Window capture failed; waiting to retry"); },
                    [](void *p, auto *, uint32_t f, uint32_t w, uint32_t h) { static_cast<Capture *>(p)->offeredDma(f,w,h); },
                    [](void *p, auto *) { static_cast<Capture *>(p)->copy(); }
                };
                hyprland_toplevel_export_frame_v1_add_listener(windowFrame, &listener, this);
            } else {
                screenFrame = zwlr_screencopy_manager_v1_capture_output(owner->screencopy, settings.cursor, output);
                static const zwlr_screencopy_frame_v1_listener listener = {
                    [](void *p, auto *, uint32_t f, uint32_t w, uint32_t h, uint32_t s) {
                        auto &c=*static_cast<Capture *>(p); c.format=f; c.width=w; c.height=h; c.stride=s;
                        if (zwlr_screencopy_manager_v1_get_version(c.owner->screencopy)<3) c.copy();
                    },
                    [](void *p, auto *, uint32_t f) { static_cast<Capture *>(p)->flags=f; },
                    [](void *p, auto *, uint32_t, uint32_t, uint32_t) { static_cast<Capture *>(p)->ready(); },
                    [](void *p, auto *) { static_cast<Capture *>(p)->fail("Display capture failed; waiting to retry"); },
                    [](void *, auto *, uint32_t, uint32_t, uint32_t, uint32_t) {},
                    [](void *p, auto *, uint32_t f, uint32_t w, uint32_t h) { static_cast<Capture *>(p)->offeredDma(f,w,h); },
                    [](void *p, auto *) { static_cast<Capture *>(p)->copy(); }
                };
                zwlr_screencopy_frame_v1_add_listener(screenFrame, &listener, this);
            }
        }
    };

    explicit Impl(std::string name) : socket(std::move(name)) {
        std::random_device random;
        nonce=std::to_string((uint64_t(random())<<32) | random());
    }
    std::string socket;
    std::string nonce;
    std::mutex mutex;
    Catalog catalog;
    std::vector<std::weak_ptr<Stream::Data>> streams;
    std::thread worker;
    std::atomic<bool> stopping{false};
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_shm *shm = nullptr;
    zwp_linux_dmabuf_v1 *dmabuf = nullptr;
    std::map<uint32_t,std::vector<uint64_t>> dmaModifiers;
    zwlr_screencopy_manager_v1 *screencopy = nullptr;
    hyprland_toplevel_export_manager_v1 *exporter = nullptr;
    zwlr_foreign_toplevel_manager_v1 *foreign = nullptr;
    wl_callback *sync = nullptr;
    bool ready = false;
    int rounds = 0;
    uint64_t windowSerial = 0;
    uint64_t generation = 0;
    std::map<uint32_t, std::unique_ptr<Output>> outputs;
    std::vector<std::unique_ptr<Window>> windows;
    std::map<Stream::Data *, std::unique_ptr<Capture>> captures;

    void ensure() {
        std::lock_guard lock(mutex);
        if (!worker.joinable()) { stopping = false; worker = std::thread([this] { run(); }); }
    }
    void publish() {
        Catalog next; next.ready = ready; next.displays = shm && screencopy; next.windows = shm && exporter && foreign;
        if (ready && !outputs.empty() && std::none_of(outputs.begin(),outputs.end(),[](const auto &entry) { return !entry.second->name.empty(); })) next.displays=false;
        for (const auto &[id, o] : outputs) if (!o->name.empty())
            next.targets.push_back({"output:" + o->name, o->description.empty() ? o->name : o->name + " · " + o->description, {}, {}, false, false});
        for (const auto &w : windows) if (!w->closed)
            next.targets.push_back({w->key, w->title.empty() ? w->appId : w->title, w->appId, w->title, true, w->focused});
        std::lock_guard lock(mutex); catalog = std::move(next);
    }
    void synchronize() {
        sync = wl_display_sync(display);
        static const wl_callback_listener listener = {[](void *p, wl_callback *cb, uint32_t) {
            auto &d=*static_cast<Impl *>(p); wl_callback_destroy(cb); d.sync=nullptr;
            if (++d.rounds < 2) d.synchronize(); else { d.ready=true; d.publish(); }
        }};
        wl_callback_add_listener(sync, &listener, this);
    }
    void addOutput(uint32_t id, uint32_t version) {
        auto output = std::make_unique<Output>(); output->owner=this; output->global=id;
        output->object=static_cast<wl_output *>(wl_registry_bind(registry,id,&wl_output_interface,std::min(version,4u)));
        // Registry numbers are connection-local and must never be persisted as
        // monitor identities. Older outputs without names use the portal.
        static const wl_output_listener listener = {
            [](void *p, auto *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *, int32_t t) {
                static_cast<Output *>(p)->pendingTransform=t>=0 && t<=7 ? engine::BufferTransform(t) : engine::BufferTransform::Normal;
            },
            [](void *, auto *, uint32_t, int32_t, int32_t, int32_t) {},
            [](void *p, auto *) { auto &o=*static_cast<Output *>(p); o.transform=o.pendingTransform; o.owner->publish(); },
            [](void *, auto *, int32_t) {},
            [](void *p, auto *, const char *name) { static_cast<Output *>(p)->name=name; },
            [](void *p, auto *, const char *description) { static_cast<Output *>(p)->description=description; }
        };
        wl_output_add_listener(output->object,&listener,output.get()); outputs[id]=std::move(output);
    }
    void addWindow(zwlr_foreign_toplevel_handle_v1 *object) {
        auto w=std::make_unique<Window>(); w->owner=this; w->object=object; w->key="window:"+nonce+":"+std::to_string(++windowSerial);
        static const zwlr_foreign_toplevel_handle_v1_listener listener = {
            [](void *p, auto *, const char *title) { static_cast<Window *>(p)->title=title; },
            [](void *p, auto *, const char *app) { static_cast<Window *>(p)->appId=app; },
            [](void *, auto *, wl_output *) {}, [](void *, auto *, wl_output *) {},
            [](void *p, auto *, wl_array *states) {
                auto &w=*static_cast<Window *>(p); w.focused=false;
                auto *begin=static_cast<const uint32_t *>(states->data);
                for (size_t n=0;n<states->size/sizeof(uint32_t);++n) if (begin[n]==ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) w.focused=true;
            },
            [](void *p, auto *) { static_cast<Window *>(p)->owner->publish(); },
            [](void *p, auto *) { auto &w=*static_cast<Window *>(p); w.closed=true; w.owner->publish(); },
            [](void *, auto *, zwlr_foreign_toplevel_handle_v1 *) {}
        };
        zwlr_foreign_toplevel_handle_v1_add_listener(object,&listener,w.get()); windows.push_back(std::move(w));
    }
    void select(Capture &c) {
        Settings settings; uint64_t version; bool stopped;
        { std::lock_guard lock(c.data->mutex); settings=c.data->settings; version=c.data->version; stopped=c.data->stopped; }
        if (stopped) { c.clear(); return; }
        wl_output *output=nullptr; zwlr_foreign_toplevel_handle_v1 *window=nullptr;
        int matches=0; engine::BufferTransform transform=engine::BufferTransform::Normal;
        if (settings.window) {
            // A chosen live handle remains selected across title changes. After
            // restart/reconnect, only an unambiguous app/title match is restored.
            for (const auto &w : windows) if (!w->closed && !settings.focused && w->key==settings.key) { window=w->object; matches=1; break; }
            if (!window) for (const auto &w : windows) if (!w->closed &&
                (settings.focused ? w->focused : !settings.appId.empty() && w->appId==settings.appId && w->title==settings.title)) { window=w->object; ++matches; }
            if (matches!=1) window=nullptr;
        } else {
            for (const auto &[id,o] : outputs) if (o->name==settings.output || "output:"+o->name==settings.key) { output=o->object; transform=o->transform; break; }
        }
        if (c.version!=version || c.output!=output || c.window!=window || c.transform!=transform) {
            c.clear(); c.resetPool(); c.version=version; c.settings=settings; c.output=output; c.window=window; c.transform=transform; c.generation=++generation; c.next=0;
            c.state(State::Waiting);
        }
        if (!ready) return;
        if (!shm || (settings.window ? !exporter || !foreign : !screencopy)) {
            c.state(State::Unsupported,"Direct capture is unavailable; use the desktop picker"); return;
        }
        if (!settings.window && !outputs.empty() && std::none_of(outputs.begin(),outputs.end(),[](const auto &entry) { return !entry.second->name.empty(); })) {
            c.state(State::Unsupported,"Display names are unavailable; use the desktop picker"); return;
        }
        if (!output && !window) { c.state(State::Waiting,matches>1 ? "Several windows match; choose one again" : settings.focused ? "Waiting for a focused window" : "Waiting for the selected capture target"); return; }
        if (c.pending() && nowNs()-c.requested>3000000000LL) c.fail("The compositor did not finish the capture");
        if (!c.pending() && nowNs()>=c.next) c.request();
    }
    void updateCaptures() {
        std::vector<std::shared_ptr<Stream::Data>> live;
        {
            std::lock_guard lock(mutex);
            streams.erase(std::remove_if(streams.begin(),streams.end(),[&](auto &weak) {
                if (auto value=weak.lock()) { live.push_back(value); return false; } return true;
            }),streams.end());
        }
        // Capture objects must not keep abandoned streams alive themselves.
        for (auto it=captures.begin();it!=captures.end();) {
            bool stopped; { std::lock_guard lock(it->second->data->mutex); stopped=it->second->data->stopped; }
            if (stopped) { it->second->clear(); it=captures.erase(it); } else ++it;
        }
        for (const auto &data : live) {
            { std::lock_guard lock(data->mutex); if (data->stopped) continue; }
            auto &capture=captures[data.get()];
            if (!capture) { capture=std::make_unique<Capture>(); capture->owner=this; capture->data=data; }
            select(*capture);
        }
        windows.erase(std::remove_if(windows.begin(),windows.end(),[](const auto &window) {
            if (!window->closed) return false;
            zwlr_foreign_toplevel_handle_v1_destroy(window->object);
            return true;
        }),windows.end());
    }
    void disconnected(const std::string &message) {
        std::lock_guard lock(mutex); catalog.ready=true; catalog.error=message; catalog.displays=catalog.windows=false; catalog.targets.clear();
        for (const auto &weak : streams) if (auto stream=weak.lock()) {
            std::lock_guard inner(stream->mutex);
            if (!stream->stopped) { stream->state=State::Unsupported; stream->error=message; stream->picture=false; stream->latest.reset(); }
        }
    }
    void run() {
        while (!stopping.load()) {
            runConnection();
            for (int i=0; i<50 && !stopping.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    void runConnection() {
        ready=false; rounds=0;
        display=wl_display_connect(socket.empty() ? nullptr : socket.c_str());
        if (!display) { disconnected("Cannot connect to the Wayland compositor"); return; }
        registry=wl_display_get_registry(display);
        static const wl_registry_listener listener = {
            [](void *p, wl_registry *, uint32_t id, const char *name, uint32_t version) {
                auto &d=*static_cast<Impl *>(p);
                if (!strcmp(name,"wl_shm")) d.shm=static_cast<wl_shm *>(wl_registry_bind(d.registry,id,&wl_shm_interface,1));
                else if (!strcmp(name,"zwp_linux_dmabuf_v1") && version>=3) {
                    // Version 3 supplies an explicit format/modifier set. Allocate
                    // on the renderer's device and let async create validate the
                    // compositor import, including cross-device configurations.
                    d.dmabuf=static_cast<zwp_linux_dmabuf_v1 *>(wl_registry_bind(d.registry,id,&zwp_linux_dmabuf_v1_interface,3));
                    static const zwp_linux_dmabuf_v1_listener listener={
                        [](void *,auto *,uint32_t) {},
                        [](void *p,auto *,uint32_t f,uint32_t hi,uint32_t lo) { static_cast<Impl *>(p)->dmaModifiers[f].push_back((uint64_t(hi)<<32)|lo); }
                    };
                    zwp_linux_dmabuf_v1_add_listener(d.dmabuf,&listener,&d);
                }
                else if (!strcmp(name,"wl_output")) d.addOutput(id,version);
                else if (!strcmp(name,"zwlr_screencopy_manager_v1")) d.screencopy=static_cast<zwlr_screencopy_manager_v1 *>(wl_registry_bind(d.registry,id,&zwlr_screencopy_manager_v1_interface,std::min(version,3u)));
                else if (!strcmp(name,"hyprland_toplevel_export_manager_v1") && version>=2) d.exporter=static_cast<hyprland_toplevel_export_manager_v1 *>(wl_registry_bind(d.registry,id,&hyprland_toplevel_export_manager_v1_interface,2));
                else if (!strcmp(name,"zwlr_foreign_toplevel_manager_v1")) {
                    d.foreign=static_cast<zwlr_foreign_toplevel_manager_v1 *>(wl_registry_bind(d.registry,id,&zwlr_foreign_toplevel_manager_v1_interface,std::min(version,3u)));
                    static const zwlr_foreign_toplevel_manager_v1_listener foreignListener = {
                        [](void *p, auto *, zwlr_foreign_toplevel_handle_v1 *w) { static_cast<Impl *>(p)->addWindow(w); },
                        [](void *p, auto *) { auto &d=*static_cast<Impl *>(p); for(auto &w:d.windows) w->closed=true; d.publish(); }
                    };
                    zwlr_foreign_toplevel_manager_v1_add_listener(d.foreign,&foreignListener,&d);
                }
            },
            [](void *p, wl_registry *, uint32_t id) {
                auto &d=*static_cast<Impl *>(p); auto found=d.outputs.find(id);
                if (found!=d.outputs.end()) {
                    // In-flight requests are cancelled before the proxy is freed.
                    for(auto &[key,c]:d.captures) if(c->output==found->second->object) { c->clear(); c->output=nullptr; c->state(State::Waiting); }
                    wl_output_destroy(found->second->object); d.outputs.erase(found); d.publish();
                }
            }
        };
        wl_registry_add_listener(registry,&listener,this); synchronize();
        const auto started=nowNs();
        while(!stopping.load()) {
            if(wl_display_dispatch_pending(display)<0) break;
            updateCaptures();
            if(!ready && nowNs()-started>3000000000LL) break;
            while(wl_display_prepare_read(display)!=0) if(wl_display_dispatch_pending(display)<0) goto done;
            const int flushed=wl_display_flush(display);
            if(flushed<0 && errno!=EAGAIN) { wl_display_cancel_read(display); break; }
            pollfd fd{wl_display_get_fd(display),short(POLLIN | (flushed<0 ? POLLOUT : 0)),0};
            int timeout=20;
            for(const auto &[key,c]:captures) if(!c->pending() && (c->output || c->window))
                timeout=std::min(timeout,int(std::clamp<int64_t>((c->next-nowNs()+999999)/1000000,1,20)));
            const int result=poll(&fd,1,timeout);
            if(result>0 && (fd.revents&POLLIN)) { if(wl_display_read_events(display)<0) break; }
            else wl_display_cancel_read(display);
            if(result<0 && errno!=EINTR) break;
            if(fd.revents&(POLLERR|POLLHUP|POLLNVAL)) break;
        }
    done:
        disconnected(stopping.load() ? "Direct capture stopped" : "The Wayland compositor disconnected");
        for(auto &[key,c]:captures) c->clear();
        captures.clear();
        if(sync) wl_callback_destroy(sync);
        for(auto &w:windows) zwlr_foreign_toplevel_handle_v1_destroy(w->object);
        windows.clear();
        for(auto &[id,o]:outputs) wl_output_destroy(o->object);
        outputs.clear();
        if(foreign) zwlr_foreign_toplevel_manager_v1_destroy(foreign);
        if(exporter) hyprland_toplevel_export_manager_v1_destroy(exporter);
        if(screencopy) zwlr_screencopy_manager_v1_destroy(screencopy);
        if(shm) wl_shm_destroy(shm);
        if(dmabuf) zwp_linux_dmabuf_v1_destroy(dmabuf);
        dmabuf=nullptr; dmaModifiers.clear();
        wl_registry_destroy(registry); wl_display_disconnect(display);
        sync=nullptr; foreign=nullptr; exporter=nullptr; screencopy=nullptr; shm=nullptr; registry=nullptr; display=nullptr;
    }
};

DirectCapture::DirectCapture(std::string socket) : d(std::make_unique<Impl>(std::move(socket))) {}
DirectCapture::~DirectCapture() { stop(); }
DirectCapture::Catalog DirectCapture::catalog() { d->ensure(); std::lock_guard lock(d->mutex); return d->catalog; }
std::shared_ptr<DirectCapture::Stream> DirectCapture::start(const Settings &settings) {
    auto data=std::make_shared<Stream::Data>(); data->settings=settings;
    { std::lock_guard lock(d->mutex); d->streams.push_back(data);
      if (!d->catalog.error.empty()) { data->state=State::Unsupported; data->error=d->catalog.error; } }
    d->ensure(); return std::shared_ptr<Stream>(new Stream(data));
}
void DirectCapture::stop() {
    d->stopping=true;
    if(d->worker.joinable()) d->worker.join();
    d->ready=false; d->rounds=0;
    std::lock_guard lock(d->mutex);
    d->catalog={};
    for(const auto &weak:d->streams) if(auto stream=weak.lock()) {
        std::lock_guard inner(stream->mutex);
        stream->stopped=true; stream->state=State::Stopped; stream->picture=false;
        stream->latest.reset(); ++stream->version;
    }
}
