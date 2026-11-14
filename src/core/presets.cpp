#include "core/presets.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <optional>
#include <tuple>

#include "output/services.h"

namespace presets {

namespace {
// The words plan() shows people, under one translation context.
struct Words {
    Q_DECLARE_TR_FUNCTIONS(Presets)
};
}  // namespace

QString scenarioId(Scenario scenario) {
    switch (scenario) {
    case Scenario::Stream: return QStringLiteral("stream");
    case Scenario::Record: return QStringLiteral("record");
    case Scenario::Tutorial: return QStringLiteral("tutorial");
    case Scenario::Edit: return QStringLiteral("edit");
    case Scenario::Replay: return QStringLiteral("replay");
    case Scenario::Camera: return QStringLiteral("camera");
    case Scenario::Light: return QStringLiteral("light");
    }
    return {};
}

bool scenarioFromId(const QString &id, Scenario *scenario) {
    for (Scenario s : scenarios())
        if (scenarioId(s) == id) {
            *scenario = s;
            return true;
        }
    return false;
}

QString scenarioName(Scenario scenario) {
    switch (scenario) {
    case Scenario::Stream: return QCoreApplication::translate("Presets", "Streaming");
    case Scenario::Record: return QCoreApplication::translate("Presets", "Recording");
    case Scenario::Tutorial: return QCoreApplication::translate("Presets", "Tutorials and screencasts");
    case Scenario::Edit: return QCoreApplication::translate("Presets", "Recording for editing");
    case Scenario::Replay: return QCoreApplication::translate("Presets", "Game replays");
    case Scenario::Camera: return QCoreApplication::translate("Presets", "Webcam and video calls");
    case Scenario::Light: return QCoreApplication::translate("Presets", "Light recording");
    }
    return {};
}

QString scenarioBlurb(Scenario scenario) {
    switch (scenario) {
    case Scenario::Stream:
        return QCoreApplication::translate("Presets", "Go live to Twitch, YouTube or your own server, at what your connection carries.");
    case Scenario::Record:
        return QCoreApplication::translate("Presets", "The best picture this machine can record, in a small file for it.");
    case Scenario::Tutorial:
        return QCoreApplication::translate("Presets", "Your screen and voice: sharp text, 30 frames a second, small files.");
    case Scenario::Edit:
        return QCoreApplication::translate("Presets", "High quality for cutting later: steady frame rate, voice and sound on their own tracks, an MP4 copy.");
    case Scenario::Replay:
        return QCoreApplication::translate("Presets", "Keeps the last 30 seconds in memory; one key saves it.");
    case Scenario::Camera:
        return QCoreApplication::translate("Presets", "Your camera, or camera and screen, as a camera other apps can pick.");
    case Scenario::Light:
        return QCoreApplication::translate("Presets", "Easy on a laptop or an older machine: hardware encoding, 30 frames a second, 1080p at most.");
    }
    return {};
}

std::vector<Scenario> scenarios() {
    return {Scenario::Stream, Scenario::Record, Scenario::Tutorial, Scenario::Edit,
            Scenario::Replay, Scenario::Camera, Scenario::Light};
}

std::vector<std::pair<int, int>> sizeLadder(int width, int height) {
    std::vector<std::pair<int, int>> out;
    if (width <= 0 || height <= 0) return out;
    const auto add = [&](int h) {
        const int w = std::max(2, int(std::lround(double(width) * h / height / 2.0)) * 2);
        if (std::none_of(out.begin(), out.end(), [&](const auto &s) { return s.second == h; })) out.push_back({w, h});
    };
    const int standard[] = {2160, 1440, 1080, 900, 720, 540, 480, 360};
    if (std::find(std::begin(standard), std::end(standard), height) == std::end(standard)) out.push_back({width & ~1, height & ~1});
    for (int h : standard)
        if (h <= height) add(h);
    return out;
}

int neededKbps(const std::string &codec, int height, int fps) {
    // 1080p60 in H.264 looks right at about 6 Mb/s; the need grows with the
    // pixel count to the power 0.75, and 30 frames take about two thirds.
    double kbps = 6000.0 * std::pow(double(height) / 1080.0, 1.5);
    if (fps <= 30) kbps *= 0.65;
    else if (fps > 60) kbps *= double(fps) / 60.0;
    if (codec == "hevc") kbps *= 0.7;
    else if (codec == "av1") kbps *= 0.6;
    return std::max(500, int(std::lround(kbps / 100.0)) * 100);
}

namespace {

int targetFps(const Survey &survey, const Choices &choices) {
    int fps = 60;
    switch (choices.scenario) {
    case Scenario::Stream: fps = choices.fastMotion ? 60 : 30; break;
    case Scenario::Tutorial:
    case Scenario::Camera:
    case Scenario::Light: fps = 30; break;
    default: break;
    }
    if (choices.display >= 0 && size_t(choices.display) < survey.displays.size()) {
        const int refresh = int(std::lround(survey.displays[size_t(choices.display)].refresh));
        if (refresh >= 24) fps = std::min(fps, refresh);
    }
    return fps;
}

Display chosenDisplay(const Survey &survey, const Choices &choices) {
    if (choices.display >= 0 && size_t(choices.display) < survey.displays.size()) return survey.displays[size_t(choices.display)];
    Display d;
    d.width = 1920;
    d.height = 1080;
    return d;
}

// The canvas: the display's own size, except where the scenario asks for less work.
std::pair<int, int> canvasFor(const Survey &survey, const Choices &choices) {
    const Display d = chosenDisplay(survey, choices);
    int w = d.width, h = d.height;
    // "1080p" is the shorter side: a portrait display keeps its 1920 height.
    if (choices.scenario == Scenario::Light && std::min(w, h) > 1080) {
        if (w < h) return {1080, int(std::lround(double(h) * 1080 / w / 2.0)) * 2};  // portrait: 1080 across
        const auto ladder = sizeLadder(w, h);
        for (const auto &s : ladder)
            if (s.second <= 1080) return s;
    }
    return {w & ~1, h & ~1};
}

// The tallest recording or stream the scenario wants: 1080p on the shorter
// side, so a portrait canvas is held to 1080 wide, not 1080 tall.
int maxHeightFor(Scenario scenario, int canvasWidth, int canvasHeight) {
    switch (scenario) {
    case Scenario::Light:
    case Scenario::Camera:
        if (canvasWidth > 0 && canvasWidth < canvasHeight)
            return canvasWidth <= 1080 ? canvasHeight : int(std::lround(double(canvasHeight) * 1080 / canvasWidth)) & ~1;
        return std::min(canvasHeight, 1080);
    default: return canvasHeight;
    }
}

const EncoderSpeed *speedOf(const Survey &survey, const std::string &key, int width, int height, int fps) {
    for (const EncoderSpeed &s : survey.speeds)
        if (s.key == key && s.width == width && s.height == height && s.fps == fps) return &s;
    return nullptr;
}

// How much faster than real time an encoder must run here: it shares the
// GPU (or the processor) with everything else the app and the game do.
double headroom(const EncoderInfo &e) { return speedHeadroom(e); }

bool codecAllowed(const std::vector<std::string> &codecs, const std::string &codec) {
    return codecs.empty() || std::find(codecs.begin(), codecs.end(), codec) != codecs.end();
}

std::vector<EncoderInfo> candidates(const Survey &survey, Purpose purpose, const std::string &container,
                                    const std::vector<std::string> &codecs, bool hardwareOnly) {
    std::vector<EncoderInfo> out;
    for (const RankedEncoder &r : rankEncoders(survey.encoders, purpose, container, survey.engine)) {
        if (!r.encoder.available || !codecAllowed(codecs, r.encoder.codec) || isEditingCodec(r.encoder.codec)) continue;
        if (hardwareOnly && !r.encoder.hardware) continue;
        out.push_back(r.encoder);
    }
    return out;
}

struct Pick {
    EncoderInfo encoder;
    int width = 0, height = 0;
    double measured = 0;
};

// The biggest size in the ladder (up to maxHeight) at which an encoder, best
// ranked first, measurably keeps up; `accept` can refuse a size (a stream's
// bandwidth).
std::optional<Pick> pick(const Survey &survey, const std::vector<EncoderInfo> &ranked,
                         const std::vector<std::pair<int, int>> &ladder, int maxHeight, int fps,
                         const std::function<bool(const EncoderInfo &, int height)> &accept = {}) {
    for (const auto &[w, h] : ladder) {
        if (h > maxHeight) continue;
        for (const EncoderInfo &e : ranked) {
            if (accept && !accept(e, h)) continue;
            const EncoderSpeed *s = speedOf(survey, encoderKey(e), w, h, fps);
            if (s && s->opened && s->framesPerSecond >= fps * headroom(e)) return Pick{e, w, h, s->framesPerSecond};
        }
    }
    return std::nullopt;
}

// Hardware first at any size it keeps up at: a processor encoder at a bigger
// size would take the processor from the game or the app being shown.
std::optional<Pick> pickPreferringHardware(const Survey &survey, const std::vector<EncoderInfo> &ranked,
                                           const std::vector<std::pair<int, int>> &ladder, int maxHeight, int fps,
                                           const std::function<bool(const EncoderInfo &, int height)> &accept = {}) {
    std::vector<EncoderInfo> hardware;
    std::copy_if(ranked.begin(), ranked.end(), std::back_inserter(hardware), [](const EncoderInfo &e) { return e.hardware; });
    if (auto p = pick(survey, hardware, ladder, maxHeight, fps, accept)) return p;
    return pick(survey, ranked, ladder, maxHeight, fps, accept);
}

// The sizes worth timing: the canvas and up to two standard steps below it.
std::vector<std::pair<int, int>> timedSizes(const std::vector<std::pair<int, int>> &ladder, int maxHeight) {
    std::vector<std::pair<int, int>> out;
    for (const auto &s : ladder)
        if (s.second <= maxHeight && out.size() < 3) out.push_back(s);
    return out;
}

QString sizeText(int w, int h) { return QStringLiteral("%1×%2").arg(w).arg(h); }

QString codecText(const std::string &codec) {
    if (codec == "h264") return QStringLiteral("H.264");
    if (codec == "hevc") return QStringLiteral("HEVC (H.265)");
    if (codec == "av1") return QStringLiteral("AV1");
    if (codec == "apv") return QStringLiteral("APV");
    return QString::fromStdString(codec);
}

int qualityFor(Scenario scenario) {
    switch (scenario) {
    case Scenario::Edit: return 16;
    case Scenario::Record: return 20;
    case Scenario::Replay: return 21;
    case Scenario::Tutorial: return 22;
    case Scenario::Light: return 25;
    default: return 23;
    }
}

bool recordsByDefault(Scenario scenario) {
    return scenario != Scenario::Stream && scenario != Scenario::Camera;
}

struct Context {
    Scenario scenario;
    int canvasW = 0, canvasH = 0, fps = 60, maxHeight = 0;
    std::vector<std::pair<int, int>> ladder;
    const StreamService *service = nullptr;
    std::vector<std::string> streamCodecs;
    int streamMaxHeight = 0, streamFps = 60;
    int streamCeiling = 0;   // the service's, kb/s; 0 = none
    int streamBudget = 0;    // what the stream may use, kb/s
    bool hardwareOnly = false;
};

Context contextFor(const Survey &survey, const Choices &choices) {
    Context c;
    c.scenario = choices.scenario;
    std::tie(c.canvasW, c.canvasH) = canvasFor(survey, choices);
    c.fps = targetFps(survey, choices);
    c.maxHeight = maxHeightFor(choices.scenario, c.canvasW, c.canvasH);
    c.ladder = sizeLadder(c.canvasW, c.canvasH);
    c.hardwareOnly = choices.scenario == Scenario::Light;
    if (choices.scenario == Scenario::Stream) {
        c.service = streamService(choices.service.toStdString());
        c.streamMaxHeight = c.canvasH;
        c.streamFps = c.fps;
        if (c.service) {
            c.streamCodecs = c.service->videoCodecs;
            if (c.service->maxHeight) c.streamMaxHeight = std::min(c.streamMaxHeight, c.service->maxHeight);
            if (c.service->maxFps) c.streamFps = std::min(c.streamFps, c.service->maxFps);
        }
        // Past 1440 a stream rarely reaches viewers as more than that.
        c.streamMaxHeight = std::min(c.streamMaxHeight, 1440);
        c.streamCeiling = c.service && c.service->maxBitrateKbps ? c.service->maxBitrateKbps : 0;
        // A quarter of the measured upload kept spare; unknown, the service's
        // ceiling or a common 6 Mb/s.
        c.streamBudget = survey.uploadKbps > 0 ? survey.uploadKbps * 3 / 4 : (c.streamCeiling ? c.streamCeiling : 6000);
        if (c.streamCeiling) c.streamBudget = std::min(c.streamBudget, c.streamCeiling);
    }
    return c;
}

}  // namespace

std::vector<SpeedJob> speedJobs(const Survey &survey, const Choices &choices) {
    const Context c = contextFor(survey, choices);
    std::vector<SpeedJob> jobs;
    // For a stream, only sizes the connection carries are worth timing.
    const auto add = [&](const std::vector<EncoderInfo> &ranked, int maxHeight, const std::vector<int> &rates, bool stream = false) {
        // The three best hardware encoders and the best processor one.
        int hardware = 0, software = 0;
        for (const EncoderInfo &e : ranked) {
            if (e.hardware ? hardware++ >= 3 : software++ >= 1) continue;
            for (int fps : rates) {
                std::vector<std::pair<int, int>> ladder;
                for (const auto &size : c.ladder)
                    if (!stream || neededKbps(e.codec, size.second, fps) <= c.streamBudget) ladder.push_back(size);
                for (const auto &[w, h] : timedSizes(ladder, maxHeight)) {
                    SpeedJob job{encoderKey(e), w, h, fps};
                    if (std::find(jobs.begin(), jobs.end(), job) == jobs.end()) jobs.push_back(job);
                }
            }
        }
    };
    if (c.scenario == Scenario::Stream) {
        std::vector<int> rates{c.streamFps};
        if (c.streamFps > 30) rates.push_back(30);
        add(candidates(survey, Purpose::Stream, {}, c.streamCodecs, false), c.streamMaxHeight, rates, true);
    } else {
        Profile p;
        p.container = QStringLiteral("mkv");
        add(candidates(survey, Purpose::Record, p.recordingFormat(), {}, c.hardwareOnly), c.maxHeight, {c.fps});
        // A machine with no hardware encoder still gets a light set-up, slowly.
        if (c.hardwareOnly && candidates(survey, Purpose::Record, p.recordingFormat(), {}, true).empty())
            add(candidates(survey, Purpose::Record, p.recordingFormat(), {}, false), std::min(c.maxHeight, 720), {c.fps});
    }
    return jobs;
}

Plan plan(const Survey &survey, const Choices &choices) {
    Plan out;
    const Context c = contextFor(survey, choices);
    Profile &p = out.profile;
    p.name = scenarioName(choices.scenario);
    p.canvasWidth = c.canvasW;
    p.canvasHeight = c.canvasH;
    p.fpsNum = c.fps;
    p.fpsDen = 1;
    // A microphone chosen by name is a source in the starter studios, listed with the camera.
    // Automatic has no one device to name, so it stays the profile's own microphone.
    const bool voice = choices.microphone != QLatin1String("none");
    const bool namedVoice = voice && !choices.microphone.isEmpty();
    p.microphone = namedVoice ? QStringLiteral("none") : choices.microphone;
    p.microphoneAdded = voice && !namedVoice;
    // Sound is added only as chosen: a camera brings its picture, and nothing
    // is heard that was not picked.
    p.desktopAdded = choices.desktopSound;
    p.container = QStringLiteral("mkv");
    p.quality = qualityFor(choices.scenario);
    p.downscaleFilter = choices.scenario == Scenario::Tutorial ? QStringLiteral("lanczos") : QStringLiteral("area");
    const Display display = chosenDisplay(survey, choices);
    const QString displayName = display.description.isEmpty() ? display.name : display.description;

    out.reasons.push_back({Words::tr("Canvas"), sizeText(c.canvasW, c.canvasH),
                           c.canvasW == display.width && c.canvasH == display.height
                               ? (displayName.isEmpty() ? Words::tr("The size of your display.") : Words::tr("The size of %1.").arg(displayName))
                               : Words::tr("Your display's shape at 1080 lines: less work for the machine.")});
    const QString fpsWhy = choices.scenario == Scenario::Tutorial ? Words::tr("Plenty for screens and speech, and half the work.")
                         : choices.scenario == Scenario::Stream && !choices.fastMotion ? Words::tr("Talking and desktops need no more; it leaves bits for detail.")
                         : choices.scenario == Scenario::Light ? Words::tr("Half the work of 60.")
                         : choices.scenario == Scenario::Camera ? Words::tr("What webcams and calls use.")
                         : Words::tr("Smooth motion, up to what your display shows.");
    out.reasons.push_back({Words::tr("Frame rate"), Words::tr("%1 frames a second").arg(c.fps), fpsWhy});

    // Recording: every scenario gets a sound one, even where it is not the point.
    const std::string container = p.recordingFormat();
    std::vector<EncoderInfo> recordRanked = candidates(survey, Purpose::Record, container, {}, c.hardwareOnly);
    int recordMax = c.maxHeight;
    if (c.hardwareOnly && recordRanked.empty()) {
        recordRanked = candidates(survey, Purpose::Record, container, {}, false);
        recordMax = std::min(recordMax, 720);
        out.warnings << Words::tr("This machine has no hardware encoder, so recording uses the processor; the light set-up keeps it to 720 lines.");
    }
    if (const auto r = pickPreferringHardware(survey, recordRanked, c.ladder, recordMax, c.fps)) {
        p.encoder = QString::fromStdString(encoderKey(r->encoder));
        p.recordingHeight = r->height == c.canvasH ? 0 : r->height;
        if (choices.scenario == Scenario::Edit) {
            p.recordingChoice.keyframeSeconds = 1;  // quick to scrub and cut anywhere
            p.recordingChoice.bFrames = 0;
        }
        QString why = Words::tr("The best encoder here that keeps up at this size: it ran at %1 frames a second.").arg(int(r->measured));
        if (r->height < c.canvasH) why += QLatin1Char(' ') + Words::tr("Larger sizes did not keep up.");
        if (recordsByDefault(choices.scenario) || choices.scenario == Scenario::Camera)
            out.reasons.push_back({Words::tr("Recording"),
                                   Words::tr("%1 %2, %3, quality %4, MKV").arg(codecText(r->encoder.codec), r->encoder.hardware ? Words::tr("(hardware)") : Words::tr("(processor)"),
                                                                      sizeText(r->width, r->height)).arg(p.quality),
                                   why});
        out.ok = true;
    } else if (choices.scenario != Scenario::Stream) {
        out.warnings << Words::tr("No encoder here kept up with %1 frames a second at any size, so nothing could be set up for recording.").arg(c.fps);
    }

    switch (choices.scenario) {
    case Scenario::Edit:
        p.remuxToMp4 = true;
        p.recordingTracks = 1 | 2 | 4;
        p.desktopAudio[QStringLiteral("tracks")] = 1 | 2;
        p.microphoneAudio[QStringLiteral("tracks")] = 1 | 4;
        out.reasons.push_back({Words::tr("Audio"), Words::tr("Three tracks: the mix, desktop sound, your microphone"),
                               Words::tr("So the voice and the sound can be balanced in the edit.")});
        out.reasons.push_back({Words::tr("Files"), Words::tr("MKV, with an MP4 copy beside each"),
                               Words::tr("MKV survives a crash; the copy opens in any editor. A keyframe every second makes cuts quick.")});
        break;
    case Scenario::Replay: {
        p.replaySeconds = 30;
        const int h = p.recordingHeight ? p.recordingHeight : c.canvasH;
        // A recording at this quality runs at roughly three times a stream's bits.
        const double kbps = 3.0 * neededKbps("h264", h, c.fps);
        int megabytes = int(std::ceil(kbps * p.replaySeconds / 8.0 / 1000.0 * 1.5));
        const int roomMb = survey.availableMemoryBytes > 0 ? int(survey.availableMemoryBytes / 8 / (1024 * 1024)) : 1024;
        megabytes = std::clamp(megabytes, 256, std::max(256, std::min(roomMb, 4096)));
        p.replayMegabytes = megabytes;
        out.reasons.push_back({Words::tr("Replay buffer"), Words::tr("The last %1 seconds, up to %2 MB").arg(p.replaySeconds).arg(megabytes),
                               Words::tr("Room for 30 seconds at this quality, within an eighth of the free memory. Ctrl+Shift+R starts it.")});
        break;
    }
    case Scenario::Camera:
        p.cameraShows = QStringLiteral("program");
        out.reasons.push_back({Words::tr("Virtual camera"), Words::tr("Shows the live studio"),
                               Words::tr("Pick OmaBroadcast as the camera in your call or browser after starting it.")});
        if (choices.camera.isEmpty()) out.warnings << Words::tr("No camera chosen: the studios show only your screen.");
        break;
    default:
        break;
    }

    if (choices.scenario == Scenario::Stream) {
        StreamTarget t;
        t.id = QStringLiteral("preset-stream");
        t.service = c.service ? QString::fromStdString(c.service->id) : QStringLiteral("custom");
        t.server = choices.server;
        if (t.server.isEmpty() && c.service && !c.service->servers.empty()) t.server = QString::fromStdString(c.service->servers.front().url);
        t.key = choices.key;
        if (c.service && c.service->keyframeSeconds) t.choice.keyframeSeconds = c.service->keyframeSeconds;
        const int ceiling = c.streamCeiling, budget = c.streamBudget;
        if (survey.uploadKbps <= 0) out.warnings << Words::tr("Your upload speed is not known, so the stream assumes %1 kb/s; a test sets it properly.").arg(budget);
        const std::vector<EncoderInfo> streamRanked = candidates(survey, Purpose::Stream, {}, c.streamCodecs, false);
        const auto fits = [&](const EncoderInfo &e, int h, int fps) { return neededKbps(e.codec, h, fps) <= budget; };
        int fps = c.streamFps;
        auto s = pickPreferringHardware(survey, streamRanked, c.ladder, c.streamMaxHeight, fps, [&](const EncoderInfo &e, int h) { return fits(e, h, fps); });
        if (!s && fps > 30) {
            fps = 30;
            s = pickPreferringHardware(survey, streamRanked, c.ladder, c.streamMaxHeight, fps, [&](const EncoderInfo &e, int h) { return fits(e, h, fps); });
            if (s) {
                p.fpsNum = 30;
                out.reasons[1] = {Words::tr("Frame rate"), Words::tr("30 frames a second"), Words::tr("Your connection carries a sharper picture at 30 than at 60.")};
            }
        }
        if (s) {
            t.encoder = QString::fromStdString(encoderKey(s->encoder));
            t.outputHeight = s->height == c.canvasH ? 0 : s->height;
            t.bitrateKbps = std::min(budget, neededKbps(s->encoder.codec, s->height, fps) * 4 / 3);
            t.bitrateKbps = std::max(t.bitrateKbps, std::min(budget, 1000));
            const QString where = c.service ? QString::fromStdString(c.service->name) : Words::tr("your server");
            QString why = survey.uploadKbps > 0
                ? (survey.uploadMeasured ? Words::tr("Your upload measured %1 kb/s; this keeps a quarter spare.") : Words::tr("You said your upload is %1 kb/s; this keeps a quarter spare.")).arg(survey.uploadKbps)
                : Words::tr("A safe guess until your upload is tested.");
            if (ceiling && t.bitrateKbps == ceiling) why += QLatin1Char(' ') + Words::tr("%1 takes no more than %2 kb/s.").arg(where).arg(ceiling);
            out.reasons.push_back({Words::tr("Stream"),
                                   Words::tr("%1 to %2, %3 at %4 kb/s").arg(codecText(s->encoder.codec), where, sizeText(s->width, s->height)).arg(t.bitrateKbps),
                                   why});
            out.ok = true;
        } else {
            out.ok = false;
            out.warnings << Words::tr("Nothing here can stream at a size your connection carries; check the upload test or choose another server.");
        }
        if (choices.key.isEmpty()) out.warnings << Words::tr("No stream key yet: add it in Settings → Streaming before going live.");
        p.streams = {t};
    }

    if (choices.scenario != Scenario::Edit && (voice || p.desktopAdded))
        out.reasons.push_back({Words::tr("Sound"),
                               voice && p.desktopAdded ? Words::tr("Your microphone and the desktop's sound")
                               : voice ? Words::tr("Your microphone") : Words::tr("The desktop's sound"),
                               p.desktopAdded ? Words::tr("Mixed on one track.") : Words::tr("Add the desktop's sound with + in Sources if you want it; it would also catch notifications.")});

    if (survey.onBattery && choices.scenario != Scenario::Light)
        out.warnings << Words::tr("This machine is on battery: Light recording uses much less power.");

    // Starter studios.
    using K = StudioItem::Kind;
    using P = StudioItem::Place;
    // Only what was chosen goes in them: no display unless it was asked for, no camera unless one was picked.
    const bool camera = !choices.camera.isEmpty();
    const bool shown = choices.screen;
    const std::vector<StudioItem> alone{{K::Camera, P::Full, {}}};
    // The display, with the camera in its corner if there is one and it is wanted there.
    const auto screen = [&](bool withCamera) {
        std::vector<StudioItem> items{{K::Display, P::Full, {}}};
        if (withCamera && camera) items.push_back({K::Camera, P::Corner, {}});
        return items;
    };
    const auto card = [&](const QString &words) { return std::vector<StudioItem>{{K::Backdrop, P::Full, {}}, {K::Title, P::Centre, words}}; };
    switch (choices.scenario) {
    case Scenario::Stream:
        out.studios = {{Words::tr("Starting soon"), card(Words::tr("Starting soon"))},
                       {Words::tr("Live"), shown ? screen(true) : camera ? alone : std::vector<StudioItem>{}},
                       {Words::tr("Be right back"), card(Words::tr("Be right back"))},
                       {Words::tr("Thanks for watching"), card(Words::tr("Thanks for watching"))}};
        break;
    case Scenario::Tutorial:
        if (shown) out.studios.push_back({Words::tr("Screen"), screen(true)});
        if (camera) out.studios.push_back({Words::tr("Camera"), alone});
        break;
    case Scenario::Replay:
        if (shown) out.studios.push_back({Words::tr("Game"), screen(true)});
        else if (camera) out.studios.push_back({Words::tr("Camera"), alone});
        break;
    case Scenario::Camera:
        if (camera) out.studios.push_back({Words::tr("Camera"), alone});
        if (shown) out.studios.push_back({camera ? Words::tr("Camera and screen") : Words::tr("Screen"), screen(true)});
        break;
    default:
        if (shown) out.studios.push_back({Words::tr("Desktop"), screen(false)});
        if (shown && camera) out.studios.push_back({Words::tr("Desktop and camera"), screen(true)});
        if (!shown && camera) out.studios.push_back({Words::tr("Camera"), alone});
        break;
    }
    // Nothing chosen to show: one studio, left empty to be filled by hand.
    if (out.studios.empty()) out.studios.push_back({Words::tr("Studio"), {}});
    // The microphone, in every studio but the title cards.
    if (namedVoice)
        for (Studio &studio : out.studios) {
            const bool titleCard = std::any_of(studio.items.begin(), studio.items.end(), [](const StudioItem &i) { return i.kind == K::Title; });
            if (!titleCard) studio.items.push_back({K::Microphone, P::Full, {}});
        }
    QStringList names;
    for (const Studio &s : out.studios) names << s.name;
    out.reasons.push_back({Words::tr("Studios"), names.join(QStringLiteral(", ")), Words::tr("A new studio collection to start from; your own are left as they are.")});
    return out;
}

}  // namespace presets
