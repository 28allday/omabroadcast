#include "encoded_stream.h"
#include "audio/format.h"
#include "engine/engine.h"

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
}

#include <time.h>

#include <algorithm>
#include <limits>
#include <cctype>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <chrono>
#include <cstring>

namespace {
constexpr size_t kMaxQueuedFrames = 6;

// FFmpeg hardware devices by type and device, made once and kept for the
// process: see warmHardwareDevice. Never torn down, so FFmpeg's Vulkan is not
// destroyed during exit.
struct HardwareDevices {
    std::mutex mutex;
    std::map<std::pair<int, std::string>, AVBufferRef *> devices;
};
HardwareDevices &hardwareDevices() {
    static auto *devices = new HardwareDevices;
    return *devices;
}

// A reference to the kept device, made now if it is not there yet.
AVBufferRef *keptDevice(AVHWDeviceType type, const std::string &device, int *result, const char *extensions = nullptr) {
    HardwareDevices &kept = hardwareDevices();
    std::lock_guard<std::mutex> lock(kept.mutex);
    const auto key = std::make_pair(int(type), device);
    if (const auto it = kept.devices.find(key); it != kept.devices.end()) return av_buffer_ref(it->second);
    AVBufferRef *made = nullptr;
    AVDictionary *options = nullptr;
    if (extensions && *extensions) av_dict_set(&options, "device_extensions", extensions, 0);
    {
        std::lock_guard<std::recursive_mutex> loader(engine::loaderMutex());
        *result = av_hwdevice_ctx_create(&made, type, device.empty() ? nullptr : device.c_str(), options, 0);
    }
    av_dict_free(&options);
    if (*result < 0) return nullptr;
    kept.devices[key] = made;
    return av_buffer_ref(made);
}

std::string avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

bool has(const std::string &name, const char *family) { return name.find(family) != std::string::npos; }

// av1_vaapi takes its number as AV1's own quantiser index, 0 to 255, where
// H.264 and HEVC read theirs as a QP of 0 to 51, so the one quality scale gets
// a table here. Each pair is a quality and the index at which AV1 gave the
// picture HEVC gives at that quality, by VMAF on game, film and screen pictures
// (Arc B580, 2026-10-30); the files come out about the same size as HEVC's.
int av1QualityIndex(int quality) {
    static constexpr int kPoints[][2] = {{0, 0},    {10, 10},   {16, 27},   {20, 50},   {23, 75},  {25, 95},
                                         {30, 140}, {35, 175},  {40, 215},  {45, 250},  {51, 255}};
    for (size_t i = 1; i < std::size(kPoints); ++i)
        if (quality <= kPoints[i][0]) {
            const int *a = kPoints[i - 1], *b = kPoints[i];
            return a[1] + (std::max(quality, a[0]) - a[0]) * (b[1] - a[1]) / (b[0] - a[0]);
        }
    return 255;
}

// The rate-control table: what each mode means on each encoder family. Our
// defaults first; a preset, tune or option the profile names wins.
bool applyRate(AVCodecContext *ctx, const EncodedStream::Settings &s, std::string *err) {
    const std::string &n = s.encoder.name;
    const VideoRate &v = s.video;
    using M = VideoRate::Mode;
    const std::string q = std::to_string(v.quality);
    const int vaapiQuality = n == "av1_vaapi" ? av1QualityIndex(v.quality) : v.quality;
    auto opt = [&](const char *key, const std::string &value) { av_opt_set(ctx->priv_data, key, value.c_str(), 0); };
    const bool nvenc = has(n, "nvenc"), vaapi = has(n, "vaapi"), vulkan = has(n, "vulkan");
    const bool x26x = n == "libx264" || n == "libx265", svt = n == "libsvtav1", aom = n == "libaom-av1";

    // An editing codec takes no rate control: its size comes from a quality
    // tier, light unless the profile names another. APV holds a constant
    // quantiser; each tier's is the one never worse than ProRes LT, 422 and
    // HQ on camera and screen pictures, at about their size on average
    // (measured 2026-10-24). An "options" qp still wins.
    if (isEditingCodec(s.encoder.codec)) {
        if (v.mode == M::Lossless) { *err = s.encoder.label + " has no lossless mode"; return false; }
        std::string tier = v.profile;
        std::transform(tier.begin(), tier.end(), tier.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const char *qp = tier.empty() || tier == "light" ? "26" : tier == "standard" ? "20" : tier == "high" ? "12" : nullptr;
        if (!qp) { *err = s.encoder.label + " takes light, standard or high, not " + v.profile; return false; }
        opt("qp", qp);
        // Pictures in flight. With one, each waits for the GPU to finish
        // before the next is uploaded, and the GPU idles through every upload.
        opt("async_depth", "4");
        for (const auto &[key, value] : parseEncoderOptions(v.options)) {
            const int r = av_opt_set(ctx->priv_data, key.c_str(), value.c_str(), 0);
            if (r < 0 && av_opt_set(ctx, key.c_str(), value.c_str(), 0) < 0) {
                *err = "option " + key + "=" + value + ": " + avErr(r);
                return false;
            }
        }
        return true;
    }

    if (v.mode == M::Cbr || v.mode == M::Vbr) {
        ctx->bit_rate = int64_t(std::max(1, v.bitrateKbps)) * 1000;
        ctx->rc_max_rate = v.mode == M::Cbr ? ctx->bit_rate
                                            : (v.maxKbps > 0 ? int64_t(v.maxKbps) * 1000 : ctx->bit_rate * 3 / 2);
        ctx->rc_buffer_size = int(v.bufferKbps > 0 ? int64_t(v.bufferKbps) * 1000 : ctx->rc_max_rate);
    }
    switch (v.mode) {
    case M::Quality:
        if (nvenc) { opt("rc", "vbr"); opt("cq", q); ctx->bit_rate = 0; }
        else if (vaapi) { opt("rc_mode", "CQP"); ctx->global_quality = vaapiQuality; }
        else if (vulkan) { opt("rc_mode", "cqp"); opt("qp", q); }
        else if (x26x || svt) opt("crf", q);
        else if (aom) { opt("crf", q); ctx->bit_rate = 0; }
        break;
    case M::Cbr:
        if (nvenc) opt("rc", "cbr");
        else if (vaapi) opt("rc_mode", "CBR");
        else if (vulkan) opt("rc_mode", "cbr");
        else if (n == "libx264") opt("nal-hrd", "cbr");
        // SVT-AV1 holds a constant rate only in its low-delay structure, and
        // takes a peak rate only in quality mode.
        else if (svt) { ctx->rc_max_rate = 0; opt("svtav1-params", "rc=2:pred-struct=1"); }
        else if (aom) opt("end-usage", "cbr");
        break;
    case M::Vbr:
        if (nvenc) opt("rc", "vbr");
        else if (vaapi) opt("rc_mode", "VBR");
        else if (vulkan) opt("rc_mode", "vbr");
        else if (svt) { ctx->rc_max_rate = 0; opt("svtav1-params", "rc=1"); }
        break;
    case M::Cqp:
        if (nvenc) { opt("rc", "constqp"); opt("qp", q); }
        else if (vaapi) { opt("rc_mode", "CQP"); ctx->global_quality = vaapiQuality; }
        else if (vulkan) { opt("rc_mode", "cqp"); opt("qp", q); }
        else if (x26x || svt) opt("qp", q);
        else if (aom) { opt("end-usage", "q"); opt("crf", q); }
        break;
    case M::Lossless:
        if (nvenc) { opt("tune", "lossless"); opt("rc", "constqp"); opt("qp", "0"); }
        else if (n == "libx264") opt("qp", "0");
        else if (n == "libx265") opt("x265-params", "lossless=1");
        else if (aom) opt("lossless", "1");
        else { *err = s.encoder.label + " has no lossless mode"; return false; }
        break;
    }
    // Our presets: hardware at its quality end, software fast enough for live.
    if (nvenc) {
        opt("preset", v.preset.empty() ? "p5" : v.preset);
        if (v.mode != M::Lossless) opt("tune", v.tune.empty() ? "hq" : v.tune);
        if (v.lookahead > 0) opt("rc-lookahead", std::to_string(v.lookahead));
    } else if (x26x) {
        opt("preset", v.preset.empty() ? "veryfast" : v.preset);
        if (n == "libx264" || !v.tune.empty()) opt("tune", v.tune.empty() ? "zerolatency" : v.tune);
        if (v.lookahead > 0) opt(n == "libx264" ? "rc-lookahead" : "x265-params",
                                 n == "libx264" ? std::to_string(v.lookahead) : "rc-lookahead=" + std::to_string(v.lookahead));
    } else if (svt) {
        opt("preset", v.preset.empty() ? "8" : v.preset);
    } else if (aom) {
        opt("cpu-used", v.preset.empty() ? "8" : v.preset);
        opt("usage", "realtime");
    } else if (!v.preset.empty()) {
        opt("preset", v.preset);
    }
    if (!v.profile.empty()) opt("profile", v.profile);
    if (!v.splitEncode.empty() && av_opt_find(ctx->priv_data, "split_encode_mode", nullptr, 0, 0) &&
        av_opt_set(ctx->priv_data, "split_encode_mode", v.splitEncode.c_str(), 0) < 0) {
        *err = "split encode " + v.splitEncode + ": not one of the encoder's";
        return false;
    }
    for (const auto &[key, value] : parseEncoderOptions(v.options)) {
        const int r = av_opt_set(ctx->priv_data, key.c_str(), value.c_str(), 0);
        if (r < 0 && av_opt_set(ctx, key.c_str(), value.c_str(), 0) < 0) {
            *err = "option " + key + "=" + value + ": " + avErr(r);
            return false;
        }
    }
    return true;
}

const AVCodec *audioEncoder(AudioCodec codec) {
    switch (codec) {
    case AudioCodec::Aac: return avcodec_find_encoder(AV_CODEC_ID_AAC);
    case AudioCodec::Opus: return avcodec_find_encoder_by_name("libopus");
    case AudioCodec::Flac: return avcodec_find_encoder(AV_CODEC_ID_FLAC);
    case AudioCodec::Alac: return avcodec_find_encoder(AV_CODEC_ID_ALAC);
    case AudioCodec::Pcm16: return avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
    case AudioCodec::Pcm24: return avcodec_find_encoder(AV_CODEC_ID_PCM_S24LE);
    case AudioCodec::PcmFloat: return avcodec_find_encoder(AV_CODEC_ID_PCM_F32LE);
    }
    return nullptr;
}

// The encoder's own sample format, preferring planar float as AAC takes it.
AVSampleFormat sampleFormatFor(const AVCodec *codec) {
    const void *configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &count) < 0 || !configs)
        return AV_SAMPLE_FMT_FLTP;
    const auto *formats = static_cast<const AVSampleFormat *>(configs);
    for (int i = 0; i < count; ++i) if (formats[i] == AV_SAMPLE_FMT_FLTP) return AV_SAMPLE_FMT_FLTP;
    for (int i = 0; i < count; ++i) if (formats[i] == AV_SAMPLE_FMT_FLT) return AV_SAMPLE_FMT_FLT;
    return formats[0];
}

// Opus runs at 48 kHz only; everything else at the mixer's rate.
int sampleRateFor(AudioCodec codec, int mixerRate) { return codec == AudioCodec::Opus ? 48000 : mixerRate; }

int64_t monotonicNow() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000ll + ts.tv_nsec;
}
}  // namespace

const char *audioCodecName(AudioCodec codec) {
    switch (codec) {
    case AudioCodec::Aac: return "aac";
    case AudioCodec::Opus: return "opus";
    case AudioCodec::Flac: return "flac";
    case AudioCodec::Alac: return "alac";
    case AudioCodec::Pcm16: return "pcm16";
    case AudioCodec::Pcm24: return "pcm24";
    case AudioCodec::PcmFloat: return "pcm32f";
    }
    return "aac";
}

AudioCodec audioCodecFromName(const std::string &name) {
    for (AudioCodec c : {AudioCodec::Aac, AudioCodec::Opus, AudioCodec::Flac, AudioCodec::Alac, AudioCodec::Pcm16,
                         AudioCodec::Pcm24, AudioCodec::PcmFloat})
        if (name == audioCodecName(c)) return c;
    return AudioCodec::Aac;
}

std::vector<std::pair<std::string, std::string>> parseEncoderOptions(const std::string &text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string word;
    bool quoted = false;
    auto flush = [&] {
        const size_t eq = word.find('=');
        if (eq != std::string::npos && eq > 0) out.emplace_back(word.substr(0, eq), word.substr(eq + 1));
        word.clear();
    };
    for (char c : text) {
        if (c == '"') { quoted = !quoted; continue; }
        if (!quoted && (c == ' ' || c == '\t' || c == '\n')) { flush(); continue; }
        word += c;
    }
    flush();
    return out;
}

bool EncodedStream::Settings::sameEncoding(const Settings &o) const {
    return encoderKey(encoder) == encoderKey(o.encoder) && encoder.swFormat == o.encoder.swFormat && output() == o.output() &&
           width == o.width && height == o.height &&
           fpsNum == o.fpsNum && fpsDen == o.fpsDen && video == o.video && audioTracks == o.audioTracks &&
           audioCodec == o.audioCodec && audioBitrateKbps == o.audioBitrateKbps && sampleRate == o.sampleRate &&
           channels == o.channels && inputChannels() == o.inputChannels();
}

engine::OutputColour EncodedStream::Settings::output() const {
    engine::OutputColour c = colour;
    if (!colourFormatOf(encoder.swFormat, &c.format)) c.format = engine::ColourFormat::NV12;
    if (engine::formatIsRgb(c.format)) c.fullRange = true;
    return c;
}

namespace {
// The tags a stream carries for its colour, as FFmpeg names them (H.273).
void tagColour(AVCodecContext *c, const engine::OutputColour &colour) {
    using S = engine::ColourSpace;
    c->color_range = colour.fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    switch (colour.space) {
    case S::Srgb: c->color_primaries = AVCOL_PRI_BT709; c->color_trc = AVCOL_TRC_IEC61966_2_1; c->colorspace = AVCOL_SPC_BT709; break;
    case S::Bt601: c->color_primaries = AVCOL_PRI_SMPTE170M; c->color_trc = AVCOL_TRC_SMPTE170M; c->colorspace = AVCOL_SPC_SMPTE170M; break;
    case S::Bt709: c->color_primaries = AVCOL_PRI_BT709; c->color_trc = AVCOL_TRC_BT709; c->colorspace = AVCOL_SPC_BT709; break;
    case S::Bt2100Pq: c->color_primaries = AVCOL_PRI_BT2020; c->color_trc = AVCOL_TRC_SMPTE2084; c->colorspace = AVCOL_SPC_BT2020_NCL; break;
    case S::Bt2100Hlg: c->color_primaries = AVCOL_PRI_BT2020; c->color_trc = AVCOL_TRC_ARIB_STD_B67; c->colorspace = AVCOL_SPC_BT2020_NCL; break;
    }
    if (engine::formatIsRgb(colour.format)) c->colorspace = AVCOL_SPC_RGB;
    // Subsampled chroma is the mean of the samples it covers: centred.
    c->chroma_sample_location = engine::chromaHalfWidth(colour.format) ? AVCHROMA_LOC_CENTER : AVCHROMA_LOC_UNSPECIFIED;
}

// HDR10's static metadata for a PQ picture: a BT.2020 D65 display reaching
// the nominal peak, and content light no brighter than it, pixel or frame
// average (the canvas is held there). Both are bounds, not measurements: a
// live picture's brightest frame is not known until it ends, and containers
// drop a light level whose frame average is left unknown (0).
void hdrMetadata(const engine::OutputColour &colour, AVMasteringDisplayMetadata *display, AVContentLightMetadata *light) {
    *display = {};
    const double xy[4][2] = {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}, {0.3127, 0.3290}};
    for (int i = 0; i < 3; ++i) {
        display->display_primaries[i][0] = av_make_q(int(xy[i][0] * 50000 + 0.5), 50000);
        display->display_primaries[i][1] = av_make_q(int(xy[i][1] * 50000 + 0.5), 50000);
    }
    display->white_point[0] = av_make_q(int(xy[3][0] * 50000 + 0.5), 50000);
    display->white_point[1] = av_make_q(int(xy[3][1] * 50000 + 0.5), 50000);
    display->max_luminance = av_make_q(int(colour.hdrPeak + 0.5f), 1);
    display->min_luminance = av_make_q(1, 10000);
    display->has_primaries = display->has_luminance = 1;
    *light = {};
    light->MaxCLL = unsigned(colour.hdrPeak + 0.5f);
    light->MaxFALL = light->MaxCLL;
}
}  // namespace

EncodedLayout::~EncodedLayout() {
    avcodec_parameters_free(&video);
    for (Audio &a : audio) avcodec_parameters_free(&a.params);
}

struct EncodedStream::Impl {
    Settings settings;
    AVCodecContext *venc = nullptr;
    AVBufferRef *hwDevice = nullptr;
    AVBufferRef *hwFrames = nullptr;
    AVFrame *swFrame = nullptr;
    AVFrame *hwFrame = nullptr;
    AVFrame *viewFrame = nullptr;    // the picture's own planes, for uploading
    engine::OutputColour colour;
    bool hdr10 = false;              // PQ: mastering display and light levels ride along
    AVMasteringDisplayMetadata display{};
    AVContentLightMetadata light{};
    int64_t startNs = 0;
    bool videoSent = false;
    int64_t lastVideoPts = -1;
    int bitrateKbps = 0;
    struct AudioTrack {
        int stream = 0;              // index in the layout, from 1
        AVCodecContext *aenc = nullptr;
        AVFrame *aframe = nullptr;
        SwrContext *swr = nullptr;
        AVAudioFifo *fifo = nullptr;
        int frameSize = 1024;
        int64_t samples = 0;
    };
    std::array<AudioTrack, 6> audioTracks;
    std::string error;
    std::function<void(const EncodedPacket &)> deliver;

    bool openVideo(EncodedLayout &layout, std::string *err) {
        const AVCodec *codec = avcodec_find_encoder_by_name(settings.encoder.name.c_str());
        if (!codec) { *err = "encoder " + settings.encoder.name + " is missing"; return false; }
        venc = avcodec_alloc_context3(codec);
        venc->width = settings.width;
        venc->height = settings.height;
        venc->time_base = {settings.fpsDen, settings.fpsNum};
        venc->framerate = {settings.fpsNum, settings.fpsDen};
        venc->gop_size = settings.video.keyframeSeconds > 0
                             ? settings.video.keyframeSeconds * settings.fpsNum / settings.fpsDen
                             : settings.fpsNum * 2 / settings.fpsDen;
        venc->max_b_frames = settings.video.bFrames;
        colour = settings.output();
        tagColour(venc, colour);
        hdr10 = colour.space == engine::ColourSpace::Bt2100Pq;
        if (hdr10) {
            hdrMetadata(colour, &display, &light);
            // For the encoders that write it into the stream themselves.
            if (auto *sd = av_frame_side_data_new(&venc->decoded_side_data, &venc->nb_decoded_side_data,
                                                  AV_FRAME_DATA_MASTERING_DISPLAY_METADATA, sizeof display, 0))
                std::memcpy(sd->data, &display, sizeof display);
            if (auto *sd = av_frame_side_data_new(&venc->decoded_side_data, &venc->nb_decoded_side_data,
                                                  AV_FRAME_DATA_CONTENT_LIGHT_LEVEL, sizeof light, 0))
                std::memcpy(sd->data, &light, sizeof light);
        }
        venc->pix_fmt = settings.encoder.swFormat;
        // Headers out of band: every file muxer wants them so, and the stream
        // muxers that want them in band put them back with a bitstream filter.
        venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        // A processor encoder on every core: libavcodec's own default is one
        // thread, which left a 4:2:2 editing encoder at 2 frames a second. A "threads" option
        // in the profile still wins, set after this.
        if (!settings.encoder.hardware) venc->thread_count = 0;
        if (!applyRate(venc, settings, err)) return false;
        bitrateKbps = int(venc->bit_rate / 1000);

        if (settings.encoder.hwType != AV_HWDEVICE_TYPE_NONE) {
            int r = 0;
            hwDevice = keptDevice(settings.encoder.hwType, settings.encoder.device, &r);
            if (!hwDevice) { *err = "hardware device: " + avErr(r); return false; }
            hwFrames = av_hwframe_ctx_alloc(hwDevice);
            auto *fc = reinterpret_cast<AVHWFramesContext *>(hwFrames->data);
            fc->format = settings.encoder.hwType == AV_HWDEVICE_TYPE_VAAPI ? AV_PIX_FMT_VAAPI : AV_PIX_FMT_VULKAN;
            fc->sw_format = settings.encoder.swFormat;
            fc->width = settings.width;
            fc->height = settings.height;
            // VA-API holds a frame per picture in flight, which options in a
            // profile can raise, and its input frames need no fixed list: no
            // limit there, or the encoder gives up when it reaches it.
            fc->initial_pool_size = settings.encoder.hwType == AV_HWDEVICE_TYPE_VAAPI ? 0 : 8;
            r = av_hwframe_ctx_init(hwFrames);
            if (r < 0) { *err = "hardware frames: " + avErr(r); return false; }
            venc->pix_fmt = fc->format;
            venc->hw_frames_ctx = av_buffer_ref(hwFrames);
            venc->hw_device_ctx = av_buffer_ref(hwDevice);
            hwFrame = av_frame_alloc();
            viewFrame = av_frame_alloc();
        }
        const int r = avcodec_open2(venc, codec, nullptr);
        if (r < 0) { *err = "video encoder: " + avErr(r); return false; }
        layout.video = avcodec_parameters_alloc();
        avcodec_parameters_from_context(layout.video, venc);
        // And for the containers: Matroska and MP4 carry them beside the stream.
        if (hdr10) {
            auto add = [&](AVPacketSideDataType type, const void *data, size_t size) {
                if (av_packet_side_data_get(layout.video->coded_side_data, layout.video->nb_coded_side_data, type)) return;
                if (AVPacketSideData *sd = av_packet_side_data_new(&layout.video->coded_side_data, &layout.video->nb_coded_side_data, type, size, 0))
                    std::memcpy(sd->data, data, size);
            };
            add(AV_PKT_DATA_MASTERING_DISPLAY_METADATA, &display, sizeof display);
            add(AV_PKT_DATA_CONTENT_LIGHT_LEVEL, &light, sizeof light);
        }
        layout.videoTimeBase = venc->time_base;
        layout.frameRate = venc->framerate;

        swFrame = av_frame_alloc();
        swFrame->format = settings.encoder.swFormat;
        swFrame->width = settings.width;
        swFrame->height = settings.height;
        if (av_frame_get_buffer(swFrame, 0) < 0) { *err = "frame buffer"; return false; }
        return true;
    }

    bool openAudioTrack(AudioTrack &track, int number, EncodedLayout &layout, std::string *err) {
        const AVCodec *codec = audioEncoder(settings.audioCodec);
        if (!codec) { *err = std::string("no ") + audioCodecName(settings.audioCodec) + " encoder"; return false; }
        const int rate = sampleRateFor(settings.audioCodec, settings.sampleRate);
        track.aenc = avcodec_alloc_context3(codec);
        track.aenc->sample_rate = rate;
        av_channel_layout_from_string(&track.aenc->ch_layout, AudioFormat::layoutName(settings.channels));
        track.aenc->sample_fmt = sampleFormatFor(codec);
        if (settings.audioCodec == AudioCodec::Aac || settings.audioCodec == AudioCodec::Opus)
            track.aenc->bit_rate = int64_t(settings.audioBitrateKbps) * 1000;
        track.aenc->time_base = {1, rate};
        track.aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary *opts = nullptr;
        // Surround Opus needs a channel mapping family.
        if (settings.audioCodec == AudioCodec::Opus && settings.channels > 2) av_dict_set(&opts, "mapping_family", "1", 0);
        int r = avcodec_open2(track.aenc, codec, &opts);
        av_dict_free(&opts);
        if (r < 0) { *err = std::string("audio encoder (") + audioCodecName(settings.audioCodec) + "): " + avErr(r); return false; }
        track.frameSize = track.aenc->frame_size > 0 ? track.aenc->frame_size : 1024;  // PCM has none of its own

        AVChannelLayout inLayout;
        // More channels coming in than the encoder takes: resampling folds them
        // down by the layouts' own matrix, centre and surrounds at -3 dB and no
        // LFE. Not scaled to fit, so a stereo source on a surround mixer keeps
        // its level.
        av_channel_layout_from_string(&inLayout, AudioFormat::layoutName(settings.inputChannels()));
        r = swr_alloc_set_opts2(&track.swr, &track.aenc->ch_layout, track.aenc->sample_fmt, rate, &inLayout,
                                AV_SAMPLE_FMT_FLT, settings.sampleRate, 0, nullptr);
        av_channel_layout_uninit(&inLayout);
        if (r < 0 || swr_init(track.swr) < 0) { *err = "resampler"; return false; }
        track.fifo = av_audio_fifo_alloc(track.aenc->sample_fmt, track.aenc->ch_layout.nb_channels, track.frameSize * 4);
        track.aframe = av_frame_alloc();
        track.aframe->format = track.aenc->sample_fmt;
        track.aframe->nb_samples = track.frameSize;
        av_channel_layout_copy(&track.aframe->ch_layout, &track.aenc->ch_layout);
        track.aframe->sample_rate = rate;
        if (av_frame_get_buffer(track.aframe, 0) < 0) { *err = "audio frame"; return false; }

        EncodedLayout::Audio a;
        a.track = number;
        a.params = avcodec_parameters_alloc();
        avcodec_parameters_from_context(a.params, track.aenc);
        a.timeBase = track.aenc->time_base;
        layout.audio.push_back(a);
        track.stream = int(layout.audio.size());
        return true;
    }

    bool drain(AVCodecContext *enc, int stream) {
        for (;;) {
            AVPacket *raw = av_packet_alloc();
            const int r = avcodec_receive_packet(enc, raw);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) { av_packet_free(&raw); return true; }
            if (r < 0) { av_packet_free(&raw); error = "encoder: " + avErr(r); return false; }
            // warmUp's pictures, at negative times, are no one's.
            if (stream == 0 && raw->pts < 0) { av_packet_free(&raw); continue; }
            EncodedPacket p;
            p.packet = std::shared_ptr<AVPacket>(raw, [](AVPacket *pkt) { av_packet_free(&pkt); });
            p.stream = stream;
            p.key = raw->flags & AV_PKT_FLAG_KEY;
            p.madeNs = monotonicNow();
            p.ptsNs = av_rescale_q(raw->pts, enc->time_base, {1, 1'000'000'000});
            p.endNs = p.ptsNs + av_rescale_q(std::max<int64_t>(raw->duration, 0), enc->time_base, {1, 1'000'000'000});
            if (stream == 0 && raw->duration <= 0)
                p.endNs = p.ptsNs + av_rescale_q(1, enc->time_base, {1, 1'000'000'000});
            deliver(p);
        }
    }

    // On every picture: some encoders write these into the stream only from here.
    void attachHdr(AVFrame *frame) {
        if (!hdr10 || av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) return;
        if (AVFrameSideData *sd = av_frame_new_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA, sizeof display))
            std::memcpy(sd->data, &display, sizeof display);
        if (AVFrameSideData *sd = av_frame_new_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL, sizeof light))
            std::memcpy(sd->data, &light, sizeof light);
    }

    bool encodeVideo(const engine::YuvFrame &y, int64_t timeNs, bool forceKey, int kbps, Stats &stats) {
        int64_t pts = av_rescale_q(std::max<int64_t>(0, timeNs - startNs), {1, 1'000'000'000}, venc->time_base);
        if (pts <= lastVideoPts) pts = lastVideoPts + 1;
        lastVideoPts = pts;
        // A new target takes effect from this frame on the encoders that
        // reconfigure on a changed bit_rate.
        if (kbps > 0 && kbps != bitrateKbps) {
            bitrateKbps = kbps;
            venc->bit_rate = int64_t(kbps) * 1000;
            if (settings.video.mode == VideoRate::Mode::Cbr) venc->rc_max_rate = venc->bit_rate;
        }

        // The picture's planes are FFmpeg's for its format, rows packed.
        if (y.format != colour.format || y.width != settings.width || y.height != settings.height) {
            error = "a picture in another format reached the encoder";
            return false;
        }
        const std::vector<uint8_t> *planes[3] = {&y.y, &y.u, &y.v};
        AVFrame *send = swFrame;
        if (hwFrames) {
            // Uploaded straight from the picture: copying it into a frame of
            // our own first took as long again (6 ms for 4K 4:2:2 10-bit). A
            // frame without buffers of its own is never kept for reading
            // later: the upload has copied it out by the time it returns.
            av_frame_unref(viewFrame);
            viewFrame->format = settings.encoder.swFormat;
            viewFrame->width = settings.width;
            viewFrame->height = settings.height;
            for (int p = 0; p < av_pix_fmt_count_planes(AVPixelFormat(viewFrame->format)) && p < 3; ++p) {
                viewFrame->data[p] = const_cast<uint8_t *>(planes[p]->data());
                viewFrame->linesize[p] = y.rowBytes(p);
            }
            av_frame_unref(hwFrame);
            if (av_hwframe_get_buffer(hwFrames, hwFrame, 0) < 0) { error = "no hardware frame"; return false; }
            const int r = av_hwframe_transfer_data(hwFrame, viewFrame, 0);
            if (r < 0) { error = "upload: " + avErr(r); return false; }
            send = hwFrame;
        } else {
            if (av_frame_make_writable(swFrame) < 0) return false;
            for (int p = 0; p < 3; ++p) {
                const int bytes = y.rowBytes(p);
                for (int row = 0; row < y.rows(p); ++row)
                    std::memcpy(swFrame->data[p] + row * swFrame->linesize[p], planes[p]->data() + size_t(row) * bytes, bytes);
            }
        }
        send->pts = pts;
        send->color_range = venc->color_range;
        send->color_primaries = venc->color_primaries;
        send->color_trc = venc->color_trc;
        send->colorspace = venc->colorspace;
        send->chroma_location = venc->chroma_sample_location;
        attachHdr(send);
        send->pict_type = forceKey ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (forceKey) send->flags |= AV_FRAME_FLAG_KEY; else send->flags &= ~AV_FRAME_FLAG_KEY;
        const auto t0 = std::chrono::steady_clock::now();
        const int r = avcodec_send_frame(venc, send);
        if (r < 0) { error = "encode: " + avErr(r); return false; }
        videoSent = true;
        if (!drain(venc, 0)) return false;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        stats.encodeMs = stats.encodeMs == 0 ? ms : stats.encodeMs * 0.95 + ms * 0.05;
        stats.framesEncoded++;
        stats.durationNs = av_rescale_q(pts, venc->time_base, {1, 1'000'000'000});
        return true;
    }

    // A picture the engine drew straight into a frame of this encoder's pool.
    bool encodeGpu(AVFrame *frame, int64_t timeNs, bool forceKey, Stats &stats) {
        int64_t pts = av_rescale_q(std::max<int64_t>(0, timeNs - startNs), {1, 1'000'000'000}, venc->time_base);
        if (pts <= lastVideoPts) pts = lastVideoPts + 1;
        lastVideoPts = pts;
        frame->pts = pts;
        frame->color_range = venc->color_range;
        frame->color_primaries = venc->color_primaries;
        frame->color_trc = venc->color_trc;
        frame->colorspace = venc->colorspace;
        frame->chroma_location = venc->chroma_sample_location;
        attachHdr(frame);
        frame->pict_type = forceKey ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (forceKey) frame->flags |= AV_FRAME_FLAG_KEY; else frame->flags &= ~AV_FRAME_FLAG_KEY;
        const auto t0 = std::chrono::steady_clock::now();
        const int r = avcodec_send_frame(venc, frame);
        av_frame_free(&frame);
        if (r < 0) { error = "encode: " + avErr(r); return false; }
        videoSent = true;
        if (!drain(venc, 0)) return false;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        stats.encodeMs = stats.encodeMs == 0 ? ms : stats.encodeMs * 0.95 + ms * 0.05;
        stats.framesEncoded++;
        stats.gpuFrames++;
        stats.durationNs = av_rescale_q(pts, venc->time_base, {1, 1'000'000'000});
        return true;
    }

    // A Vulkan encoder makes its working buffers as its first pictures need
    // them, a few tens of milliseconds each, and NVIDIA's driver holds every
    // other submission on the device meanwhile: with the compositor on the
    // same device, the first ticks of a recording ran late and lost
    // pictures. So a few unseen pictures go through first, one more than it
    // keeps in flight; what they make carries a negative time and is dropped.
    // Not with B-frames, which could lend the first real picture a time from
    // before. True when pictures went through: the encoder's own first
    // keyframe was one of them, so the first real picture must be made one.
    bool warmUp() {
        if (!hwFrames || settings.encoder.hwType != AV_HWDEVICE_TYPE_VULKAN) return false;
        if (venc->max_b_frames > 0 && !isEditingCodec(settings.encoder.codec)) return false;
        int64_t depth = 1;
        av_opt_get_int(venc->priv_data, "async_depth", 0, &depth);
        const int pictures = int(std::clamp<int64_t>(depth, 1, 8)) + 1;
        bool sent = false;
        for (int i = 0; i < pictures; ++i) {
            AVFrame *frame = av_frame_alloc();
            if (!frame || av_hwframe_get_buffer(hwFrames, frame, 0) < 0) { av_frame_free(&frame); break; }
            frame->pts = i - pictures - 1;
            const int r = avcodec_send_frame(venc, frame);
            av_frame_free(&frame);
            sent = sent || r >= 0;
            videoSent = videoSent || r >= 0;
            if (r < 0 || !drain(venc, 0)) break;  // the real pictures will say what is wrong
        }
        return sent;
    }

    bool encodeAudio(AudioTrack &track, const float *interleaved, int frames) {
        if (!track.aenc || frames <= 0) return true;
        const int outFrames = int(swr_get_out_samples(track.swr, frames));
        uint8_t *out[AV_NUM_DATA_POINTERS] = {};
        int linesize = 0;
        if (av_samples_alloc(out, &linesize, track.aenc->ch_layout.nb_channels, std::max(outFrames, 1), track.aenc->sample_fmt, 0) < 0)
            return false;
        const uint8_t *in[1] = {reinterpret_cast<const uint8_t *>(interleaved)};
        const int converted = swr_convert(track.swr, out, outFrames, in, frames);
        bool ok = converted >= 0;
        if (ok && converted > 0) av_audio_fifo_write(track.fifo, reinterpret_cast<void **>(out), converted);
        av_freep(&out[0]);
        while (ok && av_audio_fifo_size(track.fifo) >= track.frameSize) {
            av_frame_make_writable(track.aframe);
            track.aframe->nb_samples = track.frameSize;
            av_audio_fifo_read(track.fifo, reinterpret_cast<void **>(track.aframe->data), track.frameSize);
            track.aframe->pts = track.samples;
            track.samples += track.frameSize;
            const int r = avcodec_send_frame(track.aenc, track.aframe);
            if (r < 0) { error = "audio encode: " + avErr(r); return false; }
            ok = drain(track.aenc, track.stream);
        }
        return ok;
    }

    void flush() {
        // Stopped before its first picture: nothing to finish, and FFmpeg's
        // VA-API and Vulkan encoders crash on an end with no picture before it.
        if (venc && videoSent) { avcodec_send_frame(venc, nullptr); drain(venc, 0); }
        for (auto &track : audioTracks) {
            if (!track.aenc) continue;
            // Keep a partial final frame rather than drop its last samples.
            // Fixed-frame encoders get explicit zero padding.
            const int remaining = av_audio_fifo_size(track.fifo);
            if (remaining > 0 && av_frame_make_writable(track.aframe) >= 0) {
                av_samples_set_silence(track.aframe->data, 0, track.frameSize, track.aenc->ch_layout.nb_channels,
                                       track.aenc->sample_fmt);
                av_audio_fifo_read(track.fifo, reinterpret_cast<void **>(track.aframe->data), remaining);
                const bool small = (track.aenc->codec->capabilities & (AV_CODEC_CAP_SMALL_LAST_FRAME | AV_CODEC_CAP_VARIABLE_FRAME_SIZE)) ||
                                   track.aenc->frame_size == 0;
                track.aframe->nb_samples = small ? remaining : track.frameSize;
                track.aframe->pts = track.samples;
                avcodec_send_frame(track.aenc, track.aframe);
                drain(track.aenc, track.stream);
            }
            avcodec_send_frame(track.aenc, nullptr);
            drain(track.aenc, track.stream);
        }
    }

    void close() {
        std::lock_guard<std::recursive_mutex> loader(engine::loaderMutex());  // releases the hw device
        if (venc) avcodec_free_context(&venc);
        if (swFrame) av_frame_free(&swFrame);
        if (hwFrame) av_frame_free(&hwFrame);
        if (viewFrame) av_frame_free(&viewFrame);
        for (auto &track : audioTracks) {
            if (track.aenc) avcodec_free_context(&track.aenc);
            if (track.aframe) av_frame_free(&track.aframe);
            if (track.swr) swr_free(&track.swr);
            if (track.fifo) av_audio_fifo_free(track.fifo);
            track = AudioTrack{};
        }
        if (hwFrames) av_buffer_unref(&hwFrames);
        if (hwDevice) av_buffer_unref(&hwDevice);
    }
};

EncodedStream::EncodedStream() : d(new Impl) {
    d->deliver = [this](const EncodedPacket &p) {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        for (const auto &sink : m_sinks) if (!sink->done()) sink->packet(p);
    };
}

EncodedStream::~EncodedStream() {
    stop();
    dropQueued();
    {
        // It may be the last hold on the hw device, on whichever thread this is.
        std::lock_guard<std::recursive_mutex> loader(engine::loaderMutex());
        av_buffer_unref(&m_gpuFrames);
    }
    delete d;
}

bool EncodedStream::start(const Settings &settings, std::string *error) {
    stop();
    if (settings.audioTracks && (AudioFormat::validChannels(settings.channels) != settings.channels ||
                                 AudioFormat::validChannels(settings.inputChannels()) != settings.inputChannels())) {
        if (error) *error = "Unsupported audio channel layout";
        return false;
    }
    m_settings = settings;
    d->settings = settings;
    d->lastVideoPts = -1;
    d->videoSent = false;
    d->error.clear();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_video.clear();
        for (auto &track : m_audio) track.clear();
        m_stats = Stats{};
    }
    auto layout = std::make_shared<EncodedLayout>();
    std::string err;
    bool ok = d->openVideo(*layout, &err);
    const bool warmed = ok && d->warmUp();
    for (size_t i = 0; ok && i < d->audioTracks.size(); ++i)
        if (settings.audioTracks & (1u << i)) ok = d->openAudioTrack(d->audioTracks[i], int(i), *layout, &err);
    if (!ok) {
        d->close();
        if (error) *error = err;
        return false;
    }
    m_layout = layout;
    av_buffer_unref(&m_gpuFrames);
    m_gpuDevice = nullptr;
    if (d->hwFrames && settings.encoder.hwType == AV_HWDEVICE_TYPE_VULKAN) {
        m_gpuFrames = av_buffer_ref(d->hwFrames);
        m_gpuDevice = d->hwDevice->data;
    }
    m_bitrateKbps.store(0);
    m_startNs = settings.startTimeNs ? settings.startTimeNs : monotonicNow();
    d->startNs = m_startNs;
    {
        std::lock_guard<std::mutex> lock(m_keyMutex);
        m_keyAt.clear();
        // Met by the first real picture, however early its time.
        if (warmed) m_keyAt.push_back(std::numeric_limits<int64_t>::min());
    }
    m_stop.store(false);
    m_failure.clear();
    m_sinksEnded = false;
    m_running.store(true);
    m_thread = std::thread(&EncodedStream::run, this);
    return true;
}

void EncodedStream::stop() {
    if (!m_thread.joinable()) return;
    {
        // Use the waiter's mutex: an atomic flag alone can change between
        // its predicate check and sleep, losing the final wake-up.
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop.store(true);
    }
    m_wake.notify_all();
    m_thread.join();
    m_running.store(false);
}

void EncodedStream::attach(std::shared_ptr<PacketSink> sink, int64_t fromNs) {
    if (!sink) return;
    sink->begin(m_layout);
    {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        // Closing already (it gave up): nothing more is coming to it.
        if (m_sinksEnded) { sink->end(); return; }
        // Sharing from here on: back to the rate it opened at, which one
        // output's slow link had no business lowering for this one.
        const VideoRate &rate = m_settings.video;
        if (!m_sinks.empty() && (rate.mode == VideoRate::Mode::Cbr || rate.mode == VideoRate::Mode::Vbr))
            m_bitrateKbps.store(rate.bitrateKbps);
        m_sinks.push_back(std::move(sink));
    }
    requestKeyframe(fromNs);  // the first frame is one anyway
}

void EncodedStream::requestKeyframe(int64_t atNs) {
    std::lock_guard<std::mutex> lock(m_keyMutex);
    m_keyAt.push_back(atNs);
}

void EncodedStream::detach(const PacketSink *sink) {
    std::lock_guard<std::mutex> lock(m_sinkMutex);
    std::erase_if(m_sinks, [&](const auto &s) { return s.get() == sink; });
}

bool EncodedStream::setBitrate(int kbps) {
    using M = VideoRate::Mode;
    if (!encoderFollowsBitrate(m_settings.encoder.name) || kbps <= 0 ||
        (m_settings.video.mode != M::Cbr && m_settings.video.mode != M::Vbr))
        return false;
    {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        if (std::count_if(m_sinks.begin(), m_sinks.end(), [](const auto &s) { return !s->done(); }) > 1) return false;
    }
    m_bitrateKbps.store(kbps);
    return true;
}

void EncodedStream::pushVideo(engine::YuvFrame &&frame, int64_t timeNs) {
    if (!m_running.load()) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_video.size() >= kMaxQueuedFrames) {
            m_stats.framesDropped++;
            return;
        }
        m_video.push_back({std::move(frame), timeNs});
    }
    m_wake.notify_one();
}

const void *EncodedStream::hardwareDevice() const { return m_gpuDevice; }

AVFrame *EncodedStream::newGpuFrame() {
    if (!m_running.load() || !m_gpuFrames) return nullptr;
    AVFrame *frame = av_frame_alloc();
    if (av_hwframe_get_buffer(m_gpuFrames, frame, 0) < 0) {
        av_frame_free(&frame);
        // None free in its pool: this picture is dropped, as a full queue would.
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.framesDropped++;
    }
    return frame;
}

void EncodedStream::pushGpuFrame(AVFrame *frame, int64_t timeNs) {
    if (!frame) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Checked under the lock: once the encoder has let go of its queue,
        // nothing put on it would be freed.
        if (!m_running.load()) { av_frame_free(&frame); return; }
        if (m_video.size() >= kMaxQueuedFrames) {
            m_stats.framesDropped++;
            av_frame_free(&frame);
            return;
        }
        Queued q;
        q.timeNs = timeNs;
        q.gpu = frame;
        m_video.push_back(std::move(q));
    }
    m_wake.notify_one();
}

void EncodedStream::pushAudio(const float *interleaved, int frames, int track) {
    if (!m_running.load() || !interleaved || frames <= 0 || track < 0 || track >= 6 ||
        !(m_settings.audioTracks & (1u << track))) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_audio[track].insert(m_audio[track].end(), interleaved, interleaved + size_t(frames) * m_settings.inputChannels());
    }
    m_wake.notify_one();
}

EncodedStream::Stats EncodedStream::stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void EncodedStream::dropQueued() {
    std::deque<Queued> left;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        left.swap(m_video);
    }
    for (Queued &q : left) {
        if (q.gpu) av_frame_free(&q.gpu);
        else if (onFrameDone) onFrameDone(std::move(q.frame));
    }
}

std::string EncodedStream::failure() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_failure;
}

void EncodedStream::run() {
    Stats local;
    bool ok = true;
    while (ok) {
        Queued q;
        std::array<std::vector<float>, 6> audio;
        bool haveVideo = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            const auto hasAudio = [&] { return std::any_of(m_audio.begin(), m_audio.end(), [](const auto &a) { return !a.empty(); }); };
            m_wake.wait(lock, [&] { return m_stop.load() || !m_video.empty() || hasAudio(); });
            if (m_stop.load() && m_video.empty() && !hasAudio()) break;
            if (!m_video.empty()) {
                q = std::move(m_video.front());
                m_video.pop_front();
                haveVideo = true;
            }
            audio.swap(m_audio);
        }
        for (size_t track = 0; ok && track < audio.size(); ++track)
            if (!audio[track].empty())
                ok = d->encodeAudio(d->audioTracks[track], audio[track].data(), int(audio[track].size() / size_t(m_settings.inputChannels())));
        if (ok && haveVideo) {
            bool forceKey = false;
            {
                // Every request this picture is at or past is met by it.
                std::lock_guard<std::mutex> lock(m_keyMutex);
                const int64_t at = q.timeNs - m_startNs;
                forceKey = std::erase_if(m_keyAt, [at](int64_t want) { return want <= at; }) > 0;
            }
            if (q.gpu) {
                ok = d->encodeGpu(std::exchange(q.gpu, nullptr), q.timeNs, forceKey, local);
            } else {
                ok = d->encodeVideo(q.frame, q.timeNs, forceKey, m_bitrateKbps.load(), local);
                if (onFrameDone) onFrameDone(std::move(q.frame));
            }
        }
        if (q.gpu) av_frame_free(&q.gpu);  // not encoded: the sound before it failed
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const uint64_t dropped = m_stats.framesDropped;
            m_stats = local;
            m_stats.framesDropped = dropped;
        }
    }
    if (ok) d->flush();
    {
        // Not shared or fed from here on: closing takes a while.
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!ok) m_failure = d->error.empty() ? std::string("the encoder stopped") : d->error;
        m_running.store(false);
    }
    dropQueued();  // what it gave up with, so its frames go back to their pool
    {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        for (const auto &sink : m_sinks) sink->end();
        m_sinks.clear();
        m_sinksEnded = true;
    }
    d->close();
    if (!ok && onError) onError(d->error);
}

void warmHardwareDevice(const EncoderInfo &encoder) {
    if (encoder.hwType == AV_HWDEVICE_TYPE_NONE || !encoder.available) return;
    std::thread([type = encoder.hwType, device = encoder.device] {
        int result = 0;
        if (AVBufferRef *made = keptDevice(type, device, &result)) av_buffer_unref(&made);
    }).detach();
}

AVBufferRef *keptHardwareDevice(AVHWDeviceType type, const std::string &device, const char *extensions, int *result) {
    return keptDevice(type, device, result, extensions);
}

void releaseHardwareDevices(const std::function<bool(AVHWDeviceType, const std::string &)> &keep) {
    HardwareDevices &kept = hardwareDevices();
    std::lock_guard<std::mutex> lock(kept.mutex);
    std::lock_guard<std::recursive_mutex> loader(engine::loaderMutex());
    for (auto it = kept.devices.begin(); it != kept.devices.end();) {
        if (keep(AVHWDeviceType(it->first.first), it->first.second)) { ++it; continue; }
        av_buffer_unref(&it->second);
        it = kept.devices.erase(it);
    }
}

std::vector<std::pair<AVHWDeviceType, std::string>> keptHardwareDevices() {
    HardwareDevices &kept = hardwareDevices();
    std::lock_guard<std::mutex> lock(kept.mutex);
    std::vector<std::pair<AVHWDeviceType, std::string>> out;
    for (const auto &[key, device] : kept.devices) out.push_back({AVHWDeviceType(key.first), key.second});
    return out;
}
