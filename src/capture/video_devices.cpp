#include "video_devices.h"

#include <pipewire/pipewire.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/iter.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace {

const char *formatName(VideoMode::Format f) {
    switch (f) {
    case VideoMode::Format::NV12: return "NV12";
    case VideoMode::Format::I420: return "I420";
    case VideoMode::Format::YUY2: return "YUY2";
    case VideoMode::Format::MJPEG: return "MJPEG";
    case VideoMode::Format::P010: return "P010";
    case VideoMode::Format::I010: return "I010";
    }
    return "?";
}

// The values a format property offers, whatever kind of choice it is: one
// value, an enumeration, or a range (its default and its maximum).
template <typename T>
std::vector<T> choices(const spa_pod_prop *prop, uint32_t podType) {
    std::vector<T> out;
    if (!prop) return out;
    uint32_t n = 0, choice = SPA_CHOICE_None;
    const spa_pod *values = spa_pod_get_values(&prop->value, &n, &choice);
    // Values of the kind asked for only: a device can send another.
    if (!values || n == 0 || SPA_POD_TYPE(values) != podType || SPA_POD_BODY_SIZE(values) < sizeof(T)) return out;
    const T *v = static_cast<const T *>(SPA_POD_BODY(values));
    switch (choice) {
    case SPA_CHOICE_None: out.push_back(v[0]); break;
    case SPA_CHOICE_Enum:
        for (uint32_t i = 1; i < n; ++i) out.push_back(v[i]);  // v[0] repeats the default
        if (out.empty()) out.push_back(v[0]);
        break;
    case SPA_CHOICE_Range:
    case SPA_CHOICE_Step:
        out.push_back(v[0]);
        if (n > 2) out.push_back(v[2]);
        break;
    default: out.push_back(v[0]);
    }
    return out;
}

void parseFormat(const spa_pod *param, std::vector<VideoMode> *modes) {
    uint32_t mediaType = 0, mediaSubtype = 0;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 || mediaType != SPA_MEDIA_TYPE_video) return;
    std::vector<VideoMode::Format> formats;
    if (mediaSubtype == SPA_MEDIA_SUBTYPE_mjpg) {
        formats.push_back(VideoMode::Format::MJPEG);
    } else if (mediaSubtype == SPA_MEDIA_SUBTYPE_raw) {
        for (uint32_t id : choices<uint32_t>(spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_format), SPA_TYPE_Id)) {
            if (id == SPA_VIDEO_FORMAT_NV12) formats.push_back(VideoMode::Format::NV12);
            else if (id == SPA_VIDEO_FORMAT_I420) formats.push_back(VideoMode::Format::I420);
            else if (id == SPA_VIDEO_FORMAT_YUY2) formats.push_back(VideoMode::Format::YUY2);
            else if (id == SPA_VIDEO_FORMAT_P010_10LE) formats.push_back(VideoMode::Format::P010);
            else if (id == SPA_VIDEO_FORMAT_I420_10LE) formats.push_back(VideoMode::Format::I010);
        }
    }
    const auto sizes = choices<spa_rectangle>(spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_size), SPA_TYPE_Rectangle);
    auto rates = choices<spa_fraction>(spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_framerate), SPA_TYPE_Fraction);
    // A device that does not say offers whatever it runs at; ask for 30.
    if (rates.empty()) rates.push_back({30, 1});
    for (VideoMode::Format f : formats)
        for (const spa_rectangle &s : sizes)
            for (const spa_fraction &r : rates) {
                if (s.width == 0 || s.height == 0 || r.num == 0 || r.denom == 0) continue;
                VideoMode m{f, int(s.width), int(s.height), int(r.num), int(r.denom)};
                if (m.fps() > 240) continue;  // the far end of an open range, not a real rate
                if (std::find(modes->begin(), modes->end(), m) == modes->end()) modes->push_back(m);
            }
}

std::string prop(const spa_dict *props, const char *key) {
    const char *v = props ? spa_dict_lookup(props, key) : nullptr;
    return v ? v : "";
}

}  // namespace

std::string VideoMode::key() const {
    return std::to_string(width) + "x" + std::to_string(height) + "@" + std::to_string(fpsNum) + "/" +
           std::to_string(fpsDen) + ":" + formatName(format);
}

std::string VideoMode::label() const {
    char fps[16];
    const double f = this->fps();
    std::snprintf(fps, sizeof fps, f == double(int(f)) ? "%.0f" : "%.2f", f);
    return std::to_string(width) + " × " + std::to_string(height) + " · " + fps + " fps · " + formatName(format);
}

VideoMode pickVideoMode(const std::vector<VideoMode> &modes, const std::string &wanted, double canvasFps) {
    if (modes.empty()) return {};
    for (const VideoMode &m : modes)
        if (!wanted.empty() && m.key() == wanted) return m;
    const auto uncompressedFirst = [](const VideoMode &a, const VideoMode &b) {
        const bool ma = a.format == VideoMode::Format::MJPEG, mb = b.format == VideoMode::Format::MJPEG;
        return ma && !mb;
    };
    // The largest picture at the canvas's rate, when the device has one: a
    // smaller picture at that rate would trade detail the device can give.
    if (canvasFps > 0) {
        long long largest = 0;
        for (const VideoMode &m : modes) largest = std::max(largest, 1LL * m.width * m.height);
        const VideoMode *match = nullptr;
        for (const VideoMode &m : modes)
            if (1LL * m.width * m.height == largest && std::abs(m.fps() - canvasFps) < 0.01 && (!match || uncompressedFirst(*match, m)))
                match = &m;
        if (match) return *match;
    }
    // Pixels a second: a capture card offering 1440p30 beside 1080p60 is
    // upscaling a 1080p60 source, and the frames matter more than the scale.
    return *std::max_element(modes.begin(), modes.end(), [&](const VideoMode &a, const VideoMode &b) {
        const double ra = double(a.width) * a.height * a.fps(), rb = double(b.width) * b.height * b.fps();
        if (ra != rb) return ra < rb;
        const long long pa = 1LL * a.width * a.height, pb = 1LL * b.width * b.height;
        if (pa != pb) return pa < pb;
        return uncompressedFirst(a, b);  // uncompressed wins a tie
    });
}

bool evenFrameRate(double pictureRate, double canvasFps, int *num, int *den) {
    struct Rate { double fps; int num, den; };
    static const Rate rates[] = {{24000.0 / 1001, 24000, 1001}, {24, 24, 1}, {25, 25, 1}, {30000.0 / 1001, 30000, 1001}, {30, 30, 1},
                                 {50, 50, 1}, {60000.0 / 1001, 60000, 1001}, {60, 60, 1}, {100, 100, 1},
                                 {120000.0 / 1001, 120000, 1001}, {120, 120, 1}, {144, 144, 1}};
    if (pictureRate < 20 || canvasFps <= 0) return false;
    const Rate *source = nullptr;
    for (const Rate &r : rates)
        if (std::abs(pictureRate - r.fps) < 0.6 && (!source || std::abs(pictureRate - r.fps) < std::abs(pictureRate - source->fps))) source = &r;
    if (!source) return false;
    // Even: the canvas a whole number of times the source, or the source of the canvas.
    const double up = canvasFps / source->fps, down = source->fps / canvasFps;
    if (std::abs(up - std::round(up)) < 0.02 || std::abs(down - std::round(down)) < 0.02) return false;
    // The source's own rate; doubled when it is a film or video rate on a smooth canvas (25 → 50).
    const bool twice = source->fps < 31 && canvasFps >= 48 && source->fps > 24.5;
    *num = source->num * (twice ? 2 : 1);
    *den = source->den;
    return true;
}

struct VideoDevices::Impl {
    VideoDevices *owner = nullptr;
    struct Node {
        Impl *impl = nullptr;
        uint32_t id = 0;
        pw_proxy *proxy = nullptr;
        spa_hook listener{};
        VideoDevice device;
        bool isDevice = false;
        // The modes are read again only when the node says they changed
        // (the flags of its EnumFormat change), into `pending`, and replace
        // the published ones whole once PipeWire has sent them all: a state
        // change never shows a device with no modes, or only some.
        uint32_t enumFlags = UINT32_MAX;
        std::vector<VideoMode> pending;
        int pendingSeq = -1;
    };

    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_registry *registry = nullptr;
    spa_hook registryListener{};
    spa_hook coreListener{};
    std::map<uint32_t, std::unique_ptr<Node>> nodes;  // loop thread
    Changed changed;

    mutable std::mutex mutex;
    std::vector<VideoDevice> published;

    void publish() {
        std::vector<VideoDevice> list;
        for (const auto &[id, n] : nodes)
            if (n->isDevice) list.push_back(n->device);
        {
            std::lock_guard<std::mutex> lock(mutex);
            published = std::move(list);
        }
        if (changed) changed();
    }

    static void nodeInfo(void *data, const pw_node_info *info) {
        auto *n = static_cast<Node *>(data);
        if (!info) return;
        if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) && info->props) {
            const std::string api = prop(info->props, PW_KEY_DEVICE_API);
            n->isDevice = prop(info->props, PW_KEY_MEDIA_CLASS) == "Video/Source" &&
                          (api == "v4l2" || api == "libcamera");
            n->device.id = n->id;
            n->device.serial = prop(info->props, PW_KEY_OBJECT_SERIAL);
            n->device.name = prop(info->props, PW_KEY_NODE_NAME);
            n->device.description = prop(info->props, PW_KEY_NODE_DESCRIPTION);
            if (n->device.description.empty()) n->device.description = prop(info->props, PW_KEY_NODE_NICK);
            if (n->device.description.empty()) n->device.description = n->device.name;
            n->device.api = api;
            n->device.path = prop(info->props, "api.v4l2.path");
        }
        if (!n->isDevice) return;
        // (Re-)read the modes whenever the node says its formats changed.
        if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS)
            for (uint32_t i = 0; i < info->n_params; ++i) {
                if (info->params[i].id != SPA_PARAM_EnumFormat || !(info->params[i].flags & SPA_PARAM_INFO_READ) ||
                    info->params[i].flags == n->enumFlags)
                    continue;
                n->enumFlags = info->params[i].flags;
                n->pending.clear();
                pw_node_enum_params(reinterpret_cast<pw_node *>(n->proxy), 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX,
                                    nullptr);
                n->pendingSeq = pw_core_sync(n->impl->core, PW_ID_CORE, 0);
            }
        if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) n->impl->publish();
    }

    static void nodeParam(void *data, int, uint32_t id, uint32_t, uint32_t, const spa_pod *param) {
        auto *n = static_cast<Node *>(data);
        if (id != SPA_PARAM_EnumFormat || !param) return;
        parseFormat(param, &n->pending);
    }

    // Every mode asked for before this sync has come: published together.
    static void coreDone(void *data, uint32_t id, int seq) {
        auto *impl = static_cast<Impl *>(data);
        if (id != PW_ID_CORE) return;
        bool any = false;
        for (auto &[nodeId, n] : impl->nodes)
            if (n->pendingSeq == seq) {
                n->device.modes = std::exchange(n->pending, {});
                n->pendingSeq = -1;
                any = true;
            }
        if (any) impl->publish();
    }

    // The connection itself, on PipeWire's going away. What was found went with it.
    static void coreError(void *data, uint32_t id, int, int res, const char *) {
        auto *impl = static_cast<Impl *>(data);
        if (id != PW_ID_CORE || res != -EPIPE) return;
        impl->owner->m_connected.store(false);
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->published.clear();
        }
        if (impl->changed) impl->changed();
    }

    static void global(void *data, uint32_t id, uint32_t, const char *type, uint32_t version, const spa_dict *props) {
        auto *impl = static_cast<Impl *>(data);
        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
        const char *cls = props ? spa_dict_lookup(props, PW_KEY_MEDIA_CLASS) : nullptr;
        if (!cls || std::strcmp(cls, "Video/Source") != 0) return;
        auto node = std::make_unique<Node>();
        node->impl = impl;
        node->id = id;
        node->proxy = static_cast<pw_proxy *>(
            pw_registry_bind(impl->registry, id, type, std::min<uint32_t>(version, PW_VERSION_NODE), 0));
        if (!node->proxy) return;
        static const pw_node_events events = [] {
            pw_node_events e{};
            e.version = PW_VERSION_NODE_EVENTS;
            e.info = nodeInfo;
            e.param = nodeParam;
            return e;
        }();
        pw_node_add_listener(reinterpret_cast<pw_node *>(node->proxy), &node->listener, &events, node.get());
        impl->nodes[id] = std::move(node);
    }

    static void globalRemove(void *data, uint32_t id) {
        auto *impl = static_cast<Impl *>(data);
        const auto it = impl->nodes.find(id);
        if (it == impl->nodes.end()) return;
        const bool wasDevice = it->second->isDevice;
        spa_hook_remove(&it->second->listener);
        pw_proxy_destroy(it->second->proxy);
        impl->nodes.erase(it);
        if (wasDevice) impl->publish();
    }
};

VideoDevices::VideoDevices() : d(new Impl) {
    d->owner = this;
    pw_init(nullptr, nullptr);
}

VideoDevices::~VideoDevices() {
    stop();
    delete d;
}

bool VideoDevices::start(Changed changed, std::string *error) {
    stop();
    d->changed = std::move(changed);
    d->loop = pw_thread_loop_new("omabroadcast-video-devices", nullptr);
    d->context = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
    if (pw_thread_loop_start(d->loop) < 0) {
        if (error) *error = "PipeWire device loop did not start";
        stop();
        return false;
    }
    pw_thread_loop_lock(d->loop);
    d->core = pw_context_connect(d->context, nullptr, 0);
    if (!d->core) {
        pw_thread_loop_unlock(d->loop);
        if (error) *error = "No PipeWire server";
        stop();
        return false;
    }
    static const pw_core_events coreEvents = [] {
        pw_core_events e{};
        e.version = PW_VERSION_CORE_EVENTS;
        e.done = Impl::coreDone;
        e.error = Impl::coreError;
        return e;
    }();
    pw_core_add_listener(d->core, &d->coreListener, &coreEvents, d);
    d->registry = pw_core_get_registry(d->core, PW_VERSION_REGISTRY, 0);
    static const pw_registry_events events = [] {
        pw_registry_events e{};
        e.version = PW_VERSION_REGISTRY_EVENTS;
        e.global = Impl::global;
        e.global_remove = Impl::globalRemove;
        return e;
    }();
    pw_registry_add_listener(d->registry, &d->registryListener, &events, d);
    m_connected.store(true);
    pw_thread_loop_unlock(d->loop);
    return true;
}

void VideoDevices::stop() {
    m_connected.store(false);
    if (!d->loop) return;
    pw_thread_loop_lock(d->loop);
    for (auto &[id, node] : d->nodes) {
        spa_hook_remove(&node->listener);
        pw_proxy_destroy(node->proxy);
    }
    d->nodes.clear();
    if (d->registry) {
        spa_hook_remove(&d->registryListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(d->registry));
        d->registry = nullptr;
    }
    if (d->core) {
        spa_hook_remove(&d->coreListener);
        pw_core_disconnect(d->core);
        d->core = nullptr;
    }
    pw_thread_loop_unlock(d->loop);
    pw_thread_loop_stop(d->loop);
    pw_context_destroy(d->context);
    d->context = nullptr;
    pw_thread_loop_destroy(d->loop);
    d->loop = nullptr;
    std::lock_guard<std::mutex> lock(d->mutex);
    d->published.clear();
}

std::vector<VideoDevice> VideoDevices::devices() const {
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->published;
}
