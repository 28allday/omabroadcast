#include "encoders.h"
#include "file_sink.h"
#include "engine/engine.h"
#include "core/hardware.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace {

// Opens the encoder's hardware device: the one it names, or FFmpeg's default.
AVBufferRef *openDevice(const EncoderInfo &info) {
    AVBufferRef *dev = nullptr;
    const char *device = info.device.empty() ? nullptr : info.device.c_str();
    std::lock_guard<std::recursive_mutex> lock(engine::loaderMutex());
    if (av_hwdevice_ctx_create(&dev, info.hwType, device, nullptr, 0) == 0) return dev;
    return nullptr;
}

bool tryOpen(EncoderInfo &info, int width = 1280, int height = 720, int fps = 30, AVBufferRef *shared = nullptr) {
    const AVCodec *codec = avcodec_find_encoder_by_name(info.name.c_str());
    if (!codec) {
        info.note = "not in this FFmpeg";
        return false;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = {1, fps};
    ctx->framerate = {fps, 1};
    ctx->pix_fmt = info.swFormat;
    ctx->bit_rate = 4'000'000;

    AVBufferRef *device = nullptr;
    AVBufferRef *frames = nullptr;
    bool ok = false;
    if (info.hwType != AV_HWDEVICE_TYPE_NONE) {
        device = shared ? av_buffer_ref(shared) : openDevice(info);
        if (!device) {
            info.note = "no device";
            avcodec_free_context(&ctx);
            return false;
        }
        frames = av_hwframe_ctx_alloc(device);
        auto *fc = reinterpret_cast<AVHWFramesContext *>(frames->data);
        fc->format = info.hwType == AV_HWDEVICE_TYPE_VAAPI ? AV_PIX_FMT_VAAPI : AV_PIX_FMT_VULKAN;
        fc->sw_format = info.swFormat;
        fc->width = ctx->width;
        fc->height = ctx->height;
        fc->initial_pool_size = 4;
        if (av_hwframe_ctx_init(frames) < 0) {
            info.note = "no hardware frames";
        } else {
            ctx->pix_fmt = fc->format;
            ctx->hw_frames_ctx = av_buffer_ref(frames);
            ctx->hw_device_ctx = av_buffer_ref(device);
            ok = avcodec_open2(ctx, codec, nullptr) == 0;
            if (!ok) info.note = "would not open";
        }
    } else {
        ok = avcodec_open2(ctx, codec, nullptr) == 0;
        if (!ok) info.note = "would not open";
    }
    std::lock_guard<std::recursive_mutex> lock(engine::loaderMutex());  // releases the hw device
    avcodec_free_context(&ctx);
    if (frames) av_buffer_unref(&frames);
    if (device) av_buffer_unref(&device);
    return ok;
}

}  // namespace

std::vector<EncoderInfo> encoderCandidates() {
    using T = AVHWDeviceType;
    const auto nv12 = AV_PIX_FMT_NV12, yuv = AV_PIX_FMT_YUV420P, p422 = AV_PIX_FMT_YUV422P10LE;
    // name, label (codec · where it runs, in people's words, and by what: NVENC,
    // VA-API, Vulkan on a graphics card; x264, x265, SVT, libaom on the
    // processor, so hardware encoding is plain to see), codec, hardware, input format,
    // device type, device, constant quality, then available and note, which the probe fills in
    return {
        {"h264_nvenc", "H.264 · NVIDIA graphics card (NVENC)", "h264", true, nv12, T::AV_HWDEVICE_TYPE_NONE, {}, true, false, {}},
        {"hevc_nvenc", "HEVC (H.265) · NVIDIA graphics card (NVENC)", "hevc", true, nv12, T::AV_HWDEVICE_TYPE_NONE, {}, true, false, {}},
        {"av1_nvenc", "AV1 · NVIDIA graphics card (NVENC)", "av1", true, nv12, T::AV_HWDEVICE_TYPE_NONE, {}, true, false, {}},
        {"h264_vaapi", "H.264 · graphics card (VA-API)", "h264", true, nv12, T::AV_HWDEVICE_TYPE_VAAPI, {}, false, false, {}},
        {"hevc_vaapi", "HEVC (H.265) · graphics card (VA-API)", "hevc", true, nv12, T::AV_HWDEVICE_TYPE_VAAPI, {}, false, false, {}},
        {"av1_vaapi", "AV1 · graphics card (VA-API)", "av1", true, nv12, T::AV_HWDEVICE_TYPE_VAAPI, {}, false, false, {}},
        {"h264_vulkan", "H.264 · graphics card (Vulkan)", "h264", true, nv12, T::AV_HWDEVICE_TYPE_VULKAN, {}, false, false, {}},
        {"hevc_vulkan", "HEVC (H.265) · graphics card (Vulkan)", "hevc", true, nv12, T::AV_HWDEVICE_TYPE_VULKAN, {}, false, false, {}},
        // APV: an editing codec, intra-only, 10-bit 4:2:2 at a constant quantiser.
        // The encoder runs as compute shaders on any Vulkan GPU (FFmpeg 9.1 on).
        {"apv_vulkan", "APV · graphics card (Vulkan)", "apv", true, p422, T::AV_HWDEVICE_TYPE_VULKAN, {}, false, false, {}},
        {"libx264", "H.264 · processor (x264)", "h264", false, yuv, T::AV_HWDEVICE_TYPE_NONE, {}, true, false, {}},
        {"libx265", "HEVC (H.265) · processor (x265)", "hevc", false, yuv, T::AV_HWDEVICE_TYPE_NONE, {}, true, false, {}},
        {"libsvtav1", "AV1 · processor (SVT)", "av1", false, yuv, T::AV_HWDEVICE_TYPE_NONE, {}, true, false, {}},
        {"libaom-av1", "AV1 · processor (libaom)", "av1", false, yuv, T::AV_HWDEVICE_TYPE_NONE, {}, true, false, {}},
    };
}

bool isEditingCodec(const std::string &codec) { return codec == "apv"; }

namespace {

const char *codecName(const std::string &codec) {
    return codec == "av1" ? "AV1" : codec == "hevc" ? "HEVC (H.265)" : codec == "apv" ? "APV" : "H.264";
}

// Higher is better. Hardware encoders are ranked by compression for a
// recording and by acceptance for a stream; software by speed, because a
// software encoder that cannot keep up drops frames whatever it saves.
// An editing codec scores nothing: it is chosen by hand, never as "best".
int codecScore(const EncoderInfo &e, Purpose purpose) {
    if (isEditingCodec(e.codec)) return 0;
    const int av1 = e.codec == "av1", hevc = e.codec == "hevc";
    if (!e.hardware) return av1 ? 1 : hevc ? 2 : 3;
    if (purpose == Purpose::Stream) return av1 ? 1 : hevc ? 2 : 3;
    return av1 ? 3 : hevc ? 2 : 1;
}

// 2 on the compositing GPU, 1 unknown (FFmpeg picks the device), 0 elsewhere.
int locality(const EncoderInfo &e, const EngineDevice &engine) {
    if (!e.hardware || engine.name.empty()) return 1;
    // NVENC exists only on NVIDIA's cards (PCI vendor 0x10de): with a card of
    // another make drawing, it is on another card. Which NVIDIA card FFmpeg
    // picks is not known, so one of theirs drawing leaves it unknown.
    if (e.name.find("_nvenc") != std::string::npos && engine.vendorId != 0) return engine.vendorId == 0x10de ? 1 : 0;
    if (e.hwType == AV_HWDEVICE_TYPE_VULKAN) return e.device == engine.name ? 2 : 0;
    if (e.hwType == AV_HWDEVICE_TYPE_VAAPI)
        return engine.renderNode.empty() ? 1 : e.device == engine.renderNode ? 2 : 0;
    return 1;
}

std::string reasonFor(const EncoderInfo &e, Purpose purpose, const EngineDevice &engine) {
    std::string r;
    if (isEditingCodec(e.codec))
        return std::string(codecName(e.codec)) + " on the " + (e.hardware ? "graphics card" : "processor") +
               ", made for editing: large files that cut and grade easily";
    if (!e.hardware) return e.codec == "h264" ? "on the processor, the fastest there" : "on the processor, slow there";
    r = std::string("hardware ") + codecName(e.codec);
    if (purpose == Purpose::Record)
        r += e.codec == "av1"    ? ", small files for the quality"
           : e.codec == "hevc" ? ", smaller files than H.264 for the quality"
                               : ", the most widely playable";
    else
        r += e.codec == "h264" ? ", what every streaming service accepts" : "";
    if (e.constantQuality) r += ", constant quality";
    if (locality(e, engine) == 2) r += ", on the compositing GPU";
    return r;
}

}  // namespace

bool onAnotherGpu(const EncoderInfo &encoder, const EngineDevice &engine) { return locality(encoder, engine) == 0; }

AVPixelFormat pixelFormatOf(engine::ColourFormat format) {
    using F = engine::ColourFormat;
    switch (format) {
    case F::NV12: return AV_PIX_FMT_NV12;
    case F::I420: return AV_PIX_FMT_YUV420P;
    case F::I444: return AV_PIX_FMT_YUV444P;
    case F::P010: return AV_PIX_FMT_P010LE;
    case F::I010: return AV_PIX_FMT_YUV420P10LE;
    case F::P216: return AV_PIX_FMT_P216LE;
    case F::P416: return AV_PIX_FMT_P416LE;
    case F::BGRA: return AV_PIX_FMT_BGRA;
    case F::I210: return AV_PIX_FMT_YUV422P10LE;
    }
    return AV_PIX_FMT_NV12;
}

bool colourFormatOf(AVPixelFormat pixel, engine::ColourFormat *format) {
    for (int i = 0; i < engine::kColourFormats; ++i)
        if (pixelFormatOf(engine::ColourFormat(i)) == pixel) { *format = engine::ColourFormat(i); return true; }
    return false;
}

const char *colourFormatName(engine::ColourFormat format) {
    static const char *names[] = {"NV12", "I420", "I444", "P010", "I010", "P216", "P416", "BGRA", "I210"};
    static_assert(sizeof names / sizeof names[0] == engine::kColourFormats);
    return names[int(format)];
}

bool encoderTakes(const EncoderInfo &encoder, engine::ColourFormat format) {
    const AVPixelFormat pixel = pixelFormatOf(format);
    // A probe from before formats were recorded knows only the one it opened with.
    if (encoder.formats.empty()) return pixel == encoder.swFormat;
    return std::find(encoder.formats.begin(), encoder.formats.end(), pixel) != encoder.formats.end();
}

void probeFormats(EncoderInfo &encoder) {
    encoder.formats.clear();
    if (!encoder.available) return;
    // An editing codec is handed 10-bit 4:2:2 and nothing else: APV takes
    // 4:4:4 too, but only at 10 or 12 bits, which the engine does not make.
    if (isEditingCodec(encoder.codec)) { encoder.formats.push_back(encoder.swFormat); return; }
    const AVCodec *codec = avcodec_find_encoder_by_name(encoder.name.c_str());
    if (!codec) return;
    // What might open: the codec's own list, or what the device's frames can hold.
    std::vector<AVPixelFormat> listed;
    AVBufferRef *device = nullptr;
    if (encoder.hwType != AV_HWDEVICE_TYPE_NONE) {
        device = openDevice(encoder);
        if (!device) return;
        if (AVHWFramesConstraints *c = av_hwdevice_get_hwframe_constraints(device, nullptr)) {
            for (const AVPixelFormat *f = c->valid_sw_formats; f && *f != AV_PIX_FMT_NONE; ++f) listed.push_back(*f);
            av_hwframe_constraints_free(&c);
        }
    } else {
        const void *configs = nullptr;
        int count = 0;
        if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count) >= 0 && configs)
            listed.assign(static_cast<const AVPixelFormat *>(configs), static_cast<const AVPixelFormat *>(configs) + count);
    }
    for (int i = 0; i < engine::kColourFormats; ++i) {
        const AVPixelFormat pixel = pixelFormatOf(engine::ColourFormat(i));
        if (std::find(listed.begin(), listed.end(), pixel) == listed.end()) continue;
        EncoderInfo e = encoder;
        e.swFormat = pixel;
        if (pixel == encoder.swFormat || tryOpen(e, 1280, 720, 30, device)) encoder.formats.push_back(pixel);
    }
    if (device) {
        std::lock_guard<std::recursive_mutex> lock(engine::loaderMutex());
        av_buffer_unref(&device);
    }
}

bool encoderFollowsBitrate(const std::string &name) {
    return name.find("nvenc") != std::string::npos || name == "libx264";
}

std::string withoutSecrets(const std::string &line) {
    std::string out;
    const auto endOfUrl = [&](size_t from) {
        size_t end = line.find_first_of(" \t\r\n'\"<>", from);
        if (end == std::string::npos) end = line.size();
        // The sentence's own punctuation after it.
        while (end > from && std::strchr(":;,.)", line[end - 1])) --end;
        return end;
    };
    size_t at = 0;
    for (size_t scheme; (scheme = line.find("://", at)) != std::string::npos;) {
        const size_t end = endOfUrl(scheme + 3);
        out.append(line, at, scheme + 3 - at);
        std::string rest = line.substr(scheme + 3, end - scheme - 3);
        size_t path = rest.find_first_of("/?#");
        if (path == std::string::npos) path = rest.size();
        if (const size_t sign = rest.rfind('@', path); sign != std::string::npos && sign < path) {
            rest.erase(0, sign + 1);
            path -= sign + 1;
        }
        if (const size_t query = rest.find_first_of("?#", path); query != std::string::npos) rest = rest.substr(0, query) + "?...";
        size_t begin = scheme;
        while (begin > at && std::isalnum(static_cast<unsigned char>(line[begin - 1]))) --begin;
        if (line.compare(begin, 4, "rtmp") == 0 && path < rest.size()) {
            if (const size_t key = rest.find('/', path + 1); key != std::string::npos && key + 1 < rest.size()) rest = rest.substr(0, key) + "/...";
        }
        out += rest;
        at = end;
    }
    out.append(line, at, std::string::npos);
    return out;
}

namespace {
thread_local const AvLogListener *tListener = nullptr;
}  // namespace

AvLogListener::AvLogListener(std::string secret, std::atomic<bool> *key) : passphrase(std::move(secret)), wrongKey(key) { tListener = this; }
AvLogListener::~AvLogListener() { tListener = nullptr; }

void keepSecretsOutOfAvLog() {
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_callback([](void *context, int level, const char *format, va_list arguments) {
            const AvLogListener *listener = tListener;
            // Before the level: it counts however quiet the log is. "App field don't
            // match up" and "App field not found", in libavformat/rtmpproto.c.
            if (listener && listener->wrongKey && std::strncmp(format, "App field", 9) == 0) {
                listener->wrongKey->store(true);
                return;
            }
            if (level > av_log_get_level()) return;
            static thread_local int prefix = 1;
            char line[2048];
            av_log_format_line2(context, level, format, arguments, line, sizeof line, &prefix);
            std::string out = withoutSecrets(line);
            if (listener && !listener->passphrase.empty())
                for (size_t at; (at = out.find(listener->passphrase)) != std::string::npos;) out.replace(at, listener->passphrase.size(), "...");
            std::fputs(out.c_str(), stderr);
        });
    });
}

namespace {
std::mutex gQuietMutex;
int gQuiet = 0;
}  // namespace

QuietAvLog::QuietAvLog() {
    std::lock_guard<std::mutex> lock(gQuietMutex);
    if (gQuiet++ == 0) av_log_set_level(AV_LOG_FATAL);
}

QuietAvLog::~QuietAvLog() {
    std::lock_guard<std::mutex> lock(gQuietMutex);
    if (--gQuiet == 0) av_log_set_level(AV_LOG_ERROR);
}

// A graphics card as people call it, short enough for a list: without the
// maker's family name, the driver's codename or "Graphics" ("NVIDIA GeForce
// RTX 5060 Ti" → "RTX 5060 Ti", "AMD Radeon 610M (RADV RAPHAEL_MENDOCINO)" →
// "Radeon 610M", "Intel(R) Arc(tm) B580 Graphics (BMG G21)" → "Arc B580").
std::string shortGpuName(const std::string &name) {
    std::string n;
    int depth = 0;
    for (char c : name) {
        if (c == '(') ++depth;
        else if (c == ')') depth = std::max(0, depth - 1);
        else if (depth == 0) n += c;
    }
    for (const char *prefix : {"NVIDIA GeForce ", "NVIDIA ", "AMD ", "Intel Arc", "Intel "})
        if (n.rfind(prefix, 0) == 0) { n.erase(0, std::strlen(prefix) - (std::strcmp(prefix, "Intel Arc") == 0 ? 3 : 0)); break; }
    for (const char *word : {" Graphics", " GPU"}) {
        const size_t at = n.find(word);
        if (at != std::string::npos) n.erase(at, std::strlen(word));
    }
    while (!n.empty() && n.back() == ' ') n.pop_back();
    while (!n.empty() && n.front() == ' ') n.erase(0, 1);
    // Two spaces where words were taken out: one.
    for (size_t at; (at = n.find("  ")) != std::string::npos;) n.erase(at, 1);
    return n.empty() ? name : n;
}

std::vector<EncoderInfo> probeEncoders(const std::vector<engine::GpuDevice> &gpus,
                                      const std::function<void(const EncoderInfo &, bool done)> &progress,
                                      const std::function<bool(const EncoderInfo &)> &skip) {
    // Encoders narrate their test opens at info level; keep the console for real problems.
    const QuietAvLog quiet;
    std::vector<EncoderInfo> list;
    const std::vector<std::string> nodes = hardware::renderNodes();
    for (EncoderInfo e : encoderCandidates()) {
        // One candidate per device the family can run on, labelled by the GPU.
        std::vector<std::pair<std::string, std::string>> devices;  // device, GPU name
        if (e.hwType == AV_HWDEVICE_TYPE_VAAPI) {
            for (const std::string &node : nodes) {
                std::string gpu = node;
                for (const engine::GpuDevice &g : gpus)
                    if (g.renderNode == node) gpu = g.name;
                devices.push_back({node, gpu});
            }
        } else if (e.hwType == AV_HWDEVICE_TYPE_VULKAN) {
            for (const engine::GpuDevice &g : gpus)
                if (g.hardware) devices.push_back({g.name, g.name});
        } else {
            devices.push_back({std::string(), std::string()});
        }
        for (const auto &[device, gpu] : devices) {
            EncoderInfo c = e;
            c.device = device;
            // Named by the graphics card it runs on.
            if (!gpu.empty()) c.label = std::string(codecName(c.codec)) + " · " + shortGpuName(gpu) + " (" +
                                        (c.hwType == AV_HWDEVICE_TYPE_VAAPI ? "VA-API" : "Vulkan") + ")";
            if (skip && skip(c)) {
                c.note = "took the check down, so it is left out";
                list.push_back(c);
                continue;
            }
            if (progress) progress(c, false);
            c.available = tryOpen(c);
            probeFormats(c);
            if (progress) progress(c, true);
            list.push_back(c);
        }
    }
    return list;
}

double speedHeadroom(const EncoderInfo &encoder) { return encoder.hardware ? 1.3 : 1.6; }

bool encoderOpensAt(const EncoderInfo &encoder, int width, int height, int fps) {
    EncoderInfo e = encoder;
    const QuietAvLog quiet;
    return tryOpen(e, width, height, std::max(1, fps));
}

std::vector<std::string> encoderOptionValues(const std::string &encoder, const char *option) {
    const AVCodec *codec = avcodec_find_encoder_by_name(encoder.c_str());
    if (!codec || !codec->priv_class) return {};
    const AVClass *cls = codec->priv_class;
    const AVOption *o = av_opt_find(&cls, option, nullptr, 0, AV_OPT_SEARCH_FAKE_OBJ);
    if (!o || !o->unit) return {};
    std::vector<std::string> out;
    for (const AVOption *c = nullptr; (c = av_opt_next(&cls, c));)
        if (c->type == AV_OPT_TYPE_CONST && c->unit && !strcmp(c->unit, o->unit)) out.push_back(c->name);
    return out;
}

bool containerCarries(const std::string &container, const EncoderInfo &encoder) {
    // Asked of the muxer, not the codec table (the table lists AV1 for MOV, but
    // the MOV muxer refuses it when the file is opened), and that what goes in
    // comes back out as itself: formatCarries does both, and remembers.
    const AVCodec *codec = avcodec_find_encoder_by_name(encoder.name.c_str());
    return codec && formatCarries(container, codec->id);
}

std::vector<RankedEncoder> rankEncoders(const std::vector<EncoderInfo> &probed, Purpose purpose,
                                        const std::string &container, const EngineDevice &engine,
                                        const std::set<std::string> &unfit) {
    struct Scored {
        RankedEncoder ranked;
        int delivery, hardware, codec, quality, local;
        size_t order;
    };
    std::vector<Scored> scored;
    for (size_t i = 0; i < probed.size(); ++i) {
        const EncoderInfo &e = probed[i];
        if (!e.available || unfit.count(encoderKey(e))) continue;
        if (!container.empty() && !containerCarries(container, e)) continue;
        // An editing codec is for files that will be cut, not watched or sent:
        // listed last for a recording, not at all for a stream.
        if (isEditingCodec(e.codec) && purpose == Purpose::Stream) continue;
        scored.push_back({{e, reasonFor(e, purpose, engine)}, isEditingCodec(e.codec) ? 0 : 1, e.hardware ? 1 : 0,
                          codecScore(e, purpose), e.constantQuality ? 1 : 0, locality(e, engine), i});
    }
    std::stable_sort(scored.begin(), scored.end(), [&engine](const Scored &a, const Scored &b) {
        if (a.delivery != b.delivery) return a.delivery > b.delivery;
        if (a.hardware != b.hardware) return a.hardware > b.hardware;
        if (engine.keepLocal && a.local != b.local) return a.local > b.local;
        if (a.codec != b.codec) return a.codec > b.codec;
        if (a.quality != b.quality) return a.quality > b.quality;
        if (a.local != b.local) return a.local > b.local;
        return a.order < b.order;
    });
    std::vector<RankedEncoder> out;
    for (Scored &s : scored) out.push_back(std::move(s.ranked));
    return out;
}

std::string encoderKey(const EncoderInfo &encoder) {
    return encoder.device.empty() ? encoder.name : encoder.name + "@" + encoder.device;
}

EncoderInfo findEncoder(const std::vector<EncoderInfo> &probed, const std::string &key) {
    for (const EncoderInfo &e : probed)
        if (e.available && encoderKey(e) == key) return e;
    // A key saved before devices were told apart is the bare encoder name.
    for (const EncoderInfo &e : probed)
        if (e.available && e.name == key) return e;
    return {};
}
