#pragma once

#include <atomic>
#include <set>
#include <functional>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "engine/engine.h"

// What this machine can encode with, proven by opening each encoder once
// rather than by what FFmpeg was compiled with.
struct EncoderInfo {
    std::string name;        // FFmpeg encoder name, e.g. "h264_nvenc"
    std::string label;       // for the settings list
    std::string codec;       // "h264", "hevc", "av1"
    bool hardware = false;
    AVPixelFormat swFormat = AV_PIX_FMT_NV12;   // what we hand it (or upload)
    AVHWDeviceType hwType = AV_HWDEVICE_TYPE_NONE;  // device the frames must live on
    // Which device: the render node for VA-API, the Vulkan device name for
    // Vulkan video, empty where FFmpeg picks (NVENC, software).
    std::string device;
    // Whether the recorder drives it at a constant quality (bits follow the
    // picture) rather than a constant quantiser, which spends the same bits on
    // a still desktop as on motion. A row in the recorder's rate-control table.
    bool constantQuality = false;
    bool available = false;
    std::string note;        // why not, when unavailable
    // Every colour format (as FFmpeg's pixel format) it opened with in the
    // probe: what Settings → Video offers for it.
    std::vector<AVPixelFormat> formats;
};

// Our colour formats as FFmpeg names them, and back.
AVPixelFormat pixelFormatOf(engine::ColourFormat format);
bool colourFormatOf(AVPixelFormat pixel, engine::ColourFormat *format);
const char *colourFormatName(engine::ColourFormat format);   // "NV12", "P010", …
bool encoderTakes(const EncoderInfo &encoder, engine::ColourFormat format);

// The encoder families FFmpeg might offer, before any device is attached.
std::vector<EncoderInfo> encoderCandidates();
// A codec for files that will be edited rather than watched or sent (APV):
// intra-only, sized by a quality tier (light, standard, high) rather than
// rate control, never chosen as "best available".
bool isEditingCodec(const std::string &codec);
// Opens every family on every device that might carry it (each render node
// for VA-API, each Vulkan GPU for Vulkan video) on a small test
// configuration; slow-ish (hundreds of ms).
// A graphics card's name, short enough for a list ("RTX 5060 Ti", "Radeon 610M", "Arc B580").
std::string shortGpuName(const std::string &name);
// `progress`, when given, hears of each candidate as its test starts and as it
// ends. One `skip` says yes to is listed as unavailable, untried.
std::vector<EncoderInfo> probeEncoders(const std::vector<engine::GpuDevice> &gpus,
                                      const std::function<void(const EncoderInfo &, bool done)> &progress = {},
                                      const std::function<bool(const EncoderInfo &)> &skip = {});
// Whether this encoder takes a new bitrate while it runs. FFmpeg's other
// wrappers read theirs only when they open.
bool encoderFollowsBitrate(const std::string &name);
// An FFmpeg log line with what a web address in it can hide blanked: its
// sign-in and query always, and past the application name on RTMP, where the
// stream key goes.
std::string withoutSecrets(const std::string &line);
// From here on FFmpeg's own lines go to stderr through withoutSecrets.
void keepSecretsOutOfAvLog();
// A listener's worker holds one for as long as it runs, on its own thread. FFmpeg's
// lines from that thread lose the passphrase wherever it appears. FFmpeg's RTMP
// listener takes a sender whatever application it names and only warns: that
// warning, which holds both names, is never printed and sets `wrongKey` instead.
class AvLogListener {
public:
    AvLogListener(std::string passphrase, std::atomic<bool> *wrongKey);
    ~AvLogListener();
    AvLogListener(const AvLogListener &) = delete;
    AvLogListener &operator=(const AvLogListener &) = delete;
    const std::string passphrase;
    std::atomic<bool> *const wrongKey;
};
// FFmpeg's log quietened (fatal only) while one is alive on any thread, back
// to errors when the last goes: several at once never undo each other.
class QuietAvLog {
public:
    QuietAvLog();
    ~QuietAvLog();
    QuietAvLog(const QuietAvLog &) = delete;
    QuietAvLog &operator=(const QuietAvLog &) = delete;
};
// Opens the encoder at this size and rate: hardware encoders have size limits
// the small probe cannot see.
bool encoderOpensAt(const EncoderInfo &encoder, int width, int height, int fps);
// How much faster than real time an encoder must run to keep up here: it
// shares the GPU (or the processor) with everything else being shown.
double speedHeadroom(const EncoderInfo &encoder);
// Fills `formats` with every colour format the encoder opens with: those its
// codec (or, for hardware frames, its device) lists, each proven by opening.
void probeFormats(EncoderInfo &encoder);
// The named values the encoder's option takes, in its order; empty when it
// has no such option.
std::vector<std::string> encoderOptionValues(const std::string &encoder, const char *option);
// Whether the container ("mkv", "mp4", "mov") can carry what it makes.
bool containerCarries(const std::string &container, const EncoderInfo &encoder);

// What the encoder is for decides what "best" means.
//   Record: it must keep up in real time (hardware first), then the newer
//           codec, whose files are smaller for the quality or no larger (AV1,
//           HEVC, H.264); software, when it has to be, fastest first.
//   Stream: what services accept comes first (H.264, then HEVC, then AV1).
// Either way an encoder on the compositing GPU beats one on another card, and
// one that cannot take the canvas or the container is left out.
enum class Purpose { Record, Stream };

struct EngineDevice {
    std::string name;        // Vulkan device name; empty on the CPU twin
    std::string renderNode;
    // The GPU was chosen by hand or to save the battery: keep the encoding on
    // it too, before a better codec on another GPU.
    bool keepLocal = false;
    uint32_t vendorId = 0;   // its PCI vendor; 0 when not known
};

struct RankedEncoder {
    EncoderInfo encoder;
    std::string reason;      // why it ranks where it does, for people
};
std::vector<RankedEncoder> rankEncoders(const std::vector<EncoderInfo> &probed, Purpose purpose,
                                        const std::string &container, const EngineDevice &engine,
                                        const std::set<std::string> &unfit = {});

// A hardware encoder on another GPU than the one compositing: each picture is
// read back and copied across to it.
bool onAnotherGpu(const EncoderInfo &encoder, const EngineDevice &engine);

// A stable key for an encoder on its device: what a profile saves.
std::string encoderKey(const EncoderInfo &encoder);
EncoderInfo findEncoder(const std::vector<EncoderInfo> &probed, const std::string &key);
