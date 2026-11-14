#include "encoder_helper.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "output/encoded_stream.h"

extern "C" {
#include <libavutil/log.h>
}

namespace encoderhelper {
namespace {

// Drivers sometimes talk on stdout; the answer is the line after this mark.
constexpr char kMark[] = "OMABROADCAST-ENCODERS ";
constexpr char kProgress[] = "OMABROADCAST-TRYING ";
// Each bench job's result as it finishes, before the answer.
constexpr char kBenchMark[] = "OMABROADCAST-BENCH ";

QJsonObject toJson(const EncoderInfo &e) {
    QJsonArray formats;
    for (AVPixelFormat f : e.formats) formats.append(int(f));
    return QJsonObject{{QStringLiteral("name"), QString::fromStdString(e.name)},
                       {QStringLiteral("formats"), formats},
                       {QStringLiteral("label"), QString::fromStdString(e.label)},
                       {QStringLiteral("codec"), QString::fromStdString(e.codec)},
                       {QStringLiteral("hardware"), e.hardware},
                       {QStringLiteral("swFormat"), int(e.swFormat)},
                       {QStringLiteral("hwType"), int(e.hwType)},
                       {QStringLiteral("device"), QString::fromStdString(e.device)},
                       {QStringLiteral("constantQuality"), e.constantQuality},
                       {QStringLiteral("available"), e.available},
                       {QStringLiteral("note"), QString::fromStdString(e.note)}};
}

EncoderInfo fromJson(const QJsonObject &o) {
    EncoderInfo e;
    e.name = o.value(QStringLiteral("name")).toString().toStdString();
    e.label = o.value(QStringLiteral("label")).toString().toStdString();
    e.codec = o.value(QStringLiteral("codec")).toString().toStdString();
    e.hardware = o.value(QStringLiteral("hardware")).toBool();
    e.swFormat = AVPixelFormat(o.value(QStringLiteral("swFormat")).toInt());
    e.hwType = AVHWDeviceType(o.value(QStringLiteral("hwType")).toInt());
    e.device = o.value(QStringLiteral("device")).toString().toStdString();
    e.constantQuality = o.value(QStringLiteral("constantQuality")).toBool();
    e.available = o.value(QStringLiteral("available")).toBool();
    e.note = o.value(QStringLiteral("note")).toString().toStdString();
    for (const QJsonValue &f : o.value(QStringLiteral("formats")).toArray()) e.formats.push_back(AVPixelFormat(f.toInt()));
    return e;
}

QJsonArray toJson(const std::vector<EncoderInfo> &list) {
    QJsonArray out;
    for (const EncoderInfo &e : list) out.append(toJson(e));
    return out;
}

void answer(const QJsonValue &value, const char *mark = kMark) {
    const QByteArray json = value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                                            : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    std::fputs(mark, stdout);
    std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

bool find(const QByteArray &output, QJsonDocument *doc) {
    const int at = output.lastIndexOf(kMark);
    if (at < 0) return false;
    const int from = at + int(std::strlen(kMark));
    const int end = output.indexOf('\n', from);
    QJsonParseError error{};
    *doc = QJsonDocument::fromJson(output.mid(from, end < 0 ? -1 : end - from), &error);
    return error.error == QJsonParseError::NoError;
}


// --- The bench ---------------------------------------------------------------

QJsonObject toJson(const BenchJob &j) {
    return QJsonObject{{QStringLiteral("key"), QString::fromStdString(j.key)},
                       {QStringLiteral("width"), j.width},
                       {QStringLiteral("height"), j.height},
                       {QStringLiteral("fpsNum"), j.fpsNum},
                       {QStringLiteral("fpsDen"), j.fpsDen},
                       {QStringLiteral("bitrateKbps"), j.bitrateKbps}};
}

BenchJob jobFromJson(const QJsonObject &o) {
    BenchJob j;
    j.key = o.value(QStringLiteral("key")).toString().toStdString();
    j.width = o.value(QStringLiteral("width")).toInt();
    j.height = o.value(QStringLiteral("height")).toInt();
    j.fpsNum = o.value(QStringLiteral("fpsNum")).toInt(60);
    j.fpsDen = o.value(QStringLiteral("fpsDen")).toInt(1);
    j.bitrateKbps = o.value(QStringLiteral("bitrateKbps")).toInt();
    return j;
}

QJsonObject toJson(const BenchResult &r) {
    return QJsonObject{{QStringLiteral("key"), QString::fromStdString(r.key)},
                       {QStringLiteral("width"), r.width},
                       {QStringLiteral("height"), r.height},
                       {QStringLiteral("fpsNum"), r.fpsNum},
                       {QStringLiteral("fpsDen"), r.fpsDen},
                       {QStringLiteral("bitrateKbps"), r.bitrateKbps},
                       {QStringLiteral("opened"), r.opened},
                       {QStringLiteral("framesPerSecond"), r.framesPerSecond},
                       {QStringLiteral("producedKbps"), r.producedKbps},
                       {QStringLiteral("note"), QString::fromStdString(r.note)}};
}

bool resultFromJson(const QJsonValue &v, BenchResult *r) {
    if (!v.isObject()) return false;
    const QJsonObject o = v.toObject();
    // What every result carries; anything short of it is not one.
    for (const char *field : {"key", "width", "height", "opened", "framesPerSecond"})
        if (!o.contains(QLatin1String(field))) return false;
    if (!o.value(QStringLiteral("key")).isString() || !o.value(QStringLiteral("opened")).isBool() ||
        !o.value(QStringLiteral("framesPerSecond")).isDouble())
        return false;
    *r = BenchResult{};
    r->key = o.value(QStringLiteral("key")).toString().toStdString();
    r->width = o.value(QStringLiteral("width")).toInt();
    r->height = o.value(QStringLiteral("height")).toInt();
    r->fpsNum = o.value(QStringLiteral("fpsNum")).toInt(60);
    r->fpsDen = o.value(QStringLiteral("fpsDen")).toInt(1);
    r->bitrateKbps = o.value(QStringLiteral("bitrateKbps")).toInt();
    r->opened = o.value(QStringLiteral("opened")).toBool();
    r->framesPerSecond = o.value(QStringLiteral("framesPerSecond")).toDouble();
    r->producedKbps = o.value(QStringLiteral("producedKbps")).toInt();
    r->note = o.value(QStringLiteral("note")).toString().toStdString();
    return r->framesPerSecond >= 0;
}

uint32_t mix(uint32_t x, uint32_t y, uint32_t z = 0) {
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ z * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

// A sample at 8 bits as the format stores it.
uint16_t word(engine::ColourFormat f, int v8) {
    using F = engine::ColourFormat;
    switch (f) {
    case F::P010: return uint16_t((v8 << 2) << 6);
    case F::I010:
    case F::I210: return uint16_t(v8 << 2);
    case F::P216:
    case F::P416: return uint16_t(v8 * 257);
    default: return uint16_t(v8);
    }
}

void put(std::vector<uint8_t> &plane, size_t sample, int bytes, uint16_t value) {
    if (bytes == 2) {
        plane[sample * 2] = uint8_t(value & 0xff);
        plane[sample * 2 + 1] = uint8_t(value >> 8);
    } else {
        plane[sample] = uint8_t(value);
    }
}

}  // namespace

engine::YuvFrame busyPicture(int width, int height, engine::ColourFormat format, int index) {
    engine::YuvFrame f;
    f.resize(width, height, format);
    const int dx = index * 9, dy = index * 4;
    auto luma = [&](int x, int y) {
        const uint32_t X = uint32_t(x + dx), Y = uint32_t(y + dy);
        int v = 40 + int(mix(X >> 5, Y >> 5) % 160);                       // blocks
        const uint32_t glyph = mix(X >> 2, Y >> 1, 7);
        if ((glyph & 3) == 0) v += int((glyph >> 8) % 48) - 24;            // small marks
        v += int(mix(X, Y, 3) & 31) - 16;                                  // texture
        v += int((uint32_t(x + y) + uint32_t(index) * 16) & 63) / 4;       // a sweeping ramp
        v += int(mix(uint32_t(x), uint32_t(y), uint32_t(index) + 11) & 7) - 4;  // grain
        return std::clamp(v, 16, 235);
    };
    auto chroma = [&](int x, int y, uint32_t salt) {
        const uint32_t X = uint32_t(x + dx), Y = uint32_t(y + dy);
        int v = 128 + int(mix(X >> 4, Y >> 4, salt) % 96) - 48 + int(mix(X, Y, salt + 1) & 15) - 8;
        return std::clamp(v, 16, 240);
    };
    if (engine::formatIsRgb(format)) {
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const int l = luma(x, y), cb = chroma(x, y, 21) - 128, cr = chroma(x, y, 42) - 128;
                uint8_t *p = f.y.data() + (size_t(y) * width + x) * 4;
                p[0] = uint8_t(std::clamp(l + cb * 2, 0, 255));
                p[1] = uint8_t(std::clamp(l - (cb + cr) / 2, 0, 255));
                p[2] = uint8_t(std::clamp(l + cr * 3 / 2, 0, 255));
                p[3] = 255;
            }
        return f;
    }
    const int bytes = f.sampleBytes();
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) put(f.y, size_t(y) * width + x, bytes, word(format, luma(x, y)));
    const int cw = f.chromaWidth(), ch = f.chromaHeight();
    const int sx = engine::chromaHalfWidth(format) ? 2 : 1, sy = engine::chromaHalfHeight(format) ? 2 : 1;
    const bool interleaved = engine::chromaInterleaved(format);
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const uint16_t cb = word(format, chroma(x * sx, y * sy, 21)), cr = word(format, chroma(x * sx, y * sy, 42));
            if (interleaved) {
                put(f.u, (size_t(y) * cw + x) * 2, bytes, cb);
                put(f.u, (size_t(y) * cw + x) * 2 + 1, bytes, cr);
            } else {
                put(f.u, size_t(y) * cw + x, bytes, cb);
                put(f.v, size_t(y) * cw + x, bytes, cr);
            }
        }
    return f;
}

namespace {

// Counts what the encoder makes; keeps nothing.
class CountingSink : public PacketSink {
public:
    void begin(std::shared_ptr<const EncodedLayout>) override {}
    void packet(const EncodedPacket &p) override {
        if (p.stream == 0) bytes.fetch_add(uint64_t(p.packet->size));
    }
    void end() override {}
    bool done() const override { return false; }
    std::atomic<uint64_t> bytes{0};
};

constexpr int kBenchFrameCap = 5000;
constexpr int kBenchPool = 6;        // pictures in turn
constexpr double kFitSeconds = 0.4;  // the fit's timing of each hardware encoder
constexpr int kBenchInFlight = 3;    // pushed and not yet taken: the queue never drops

BenchResult benchJob(const std::vector<EncoderInfo> &encoders, const BenchJob &job, double seconds) {
    BenchResult r;
    r.key = job.key;
    r.width = job.width;
    r.height = job.height;
    r.fpsNum = job.fpsNum;
    r.fpsDen = job.fpsDen;
    r.bitrateKbps = job.bitrateKbps;
    const EncoderInfo encoder = findEncoder(encoders, job.key);
    if (encoder.name.empty()) {
        r.note = "no such encoder here";
        return r;
    }
    if (job.width <= 0 || job.height <= 0 || job.fpsNum <= 0 || job.fpsDen <= 0) {
        r.note = "no size or rate";
        return r;
    }
    EncodedStream::Settings s;
    s.encoder = encoder;
    s.width = job.width;
    s.height = job.height;
    s.fpsNum = job.fpsNum;
    s.fpsDen = job.fpsDen;
    s.audioTracks = 0;
    if (job.bitrateKbps > 0) {
        s.video.mode = VideoRate::Mode::Cbr;
        s.video.bitrateKbps = job.bitrateKbps;
    }
    std::mutex mutex;
    std::deque<engine::YuvFrame> free;
    std::string failure;
    EncodedStream stream;
    stream.onFrameDone = [&](engine::YuvFrame &&f) {
        std::lock_guard<std::mutex> lock(mutex);
        free.push_back(std::move(f));
    };
    stream.onError = [&](const std::string &why) {
        std::lock_guard<std::mutex> lock(mutex);
        failure = why;
    };
    std::string error;
    av_log_set_level(AV_LOG_FATAL);
    const bool opened = stream.start(s, &error);
    if (!opened) {
        av_log_set_level(AV_LOG_ERROR);
        r.note = error.empty() ? "would not open" : error;
        return r;
    }
    auto counter = std::make_shared<CountingSink>();
    stream.attach(counter);
    // Made after the open, outside the timing.
    const engine::ColourFormat format = s.output().format;
    // Fewer of them for a giant size: a quarter of a gigabyte at most.
    engine::YuvFrame probe;
    probe.width = job.width;
    probe.height = job.height;
    probe.format = format;
    const size_t bytes = size_t(probe.rowBytes(0)) * probe.rows(0) + size_t(probe.rowBytes(1)) * probe.rows(1) +
                         size_t(probe.rowBytes(2)) * probe.rows(2);
    const int pool = int(std::clamp<size_t>((size_t(256) << 20) / std::max<size_t>(bytes, 1), 2, kBenchPool));
    for (int i = 0; i < pool; ++i) free.push_back(busyPicture(job.width, job.height, format, i));

    using Clock = std::chrono::steady_clock;
    const int64_t frameNs = int64_t(1'000'000'000) * job.fpsDen / job.fpsNum;
    const auto start = Clock::now();
    auto first = start, end = start;
    uint64_t pushed = 0, firstCount = 0, encoded = 0;
    for (;;) {
        const EncodedStream::Stats st = stream.stats();
        encoded = st.framesEncoded;
        const auto now = Clock::now();
        if (encoded > 0 && firstCount == 0) {
            first = now;
            firstCount = encoded;
        }
        end = now;
        if (!stream.running()) break;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!failure.empty()) break;
        }
        if (std::chrono::duration<double>(now - start).count() >= seconds || encoded >= uint64_t(kBenchFrameCap)) break;
        engine::YuvFrame f;
        bool have = false;
        if (pushed - encoded - st.framesDropped < uint64_t(kBenchInFlight)) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!free.empty()) {
                f = std::move(free.front());
                free.pop_front();
                have = true;
            }
        }
        if (have) {
            stream.pushVideo(std::move(f), stream.startTimeNs() + int64_t(pushed) * frameNs);
            ++pushed;
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    stream.stop();
    av_log_set_level(AV_LOG_ERROR);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failure.empty() || encoded == 0) {
            r.note = failure.empty() ? "encoded nothing" : failure;
            return r;
        }
    }
    r.opened = true;
    // Flushed by the stop, so every frame pushed is in the count.
    r.producedKbps = int(double(counter->bytes.load()) * 8.0 / 1000.0 / (double(pushed) * job.fpsDen / job.fpsNum) + 0.5);
    // From the first frame out: the first one carries the encoder's own
    // start-up. Too few for that, the whole run.
    const double after = std::chrono::duration<double>(end - first).count();
    if (encoded > firstCount + 1 && after > 0)
        r.framesPerSecond = double(encoded - firstCount) / after;
    else
        r.framesPerSecond = double(encoded) / std::max(1e-6, std::chrono::duration<double>(end - start).count());
    return r;
}

int runBench(int argc, char **argv) {
    const double seconds = argc > 2 ? std::atof(argv[2]) : 2.0;
    QFile in;
    if (!in.open(stdin, QIODevice::ReadOnly)) return 2;
    const QJsonObject input = QJsonDocument::fromJson(in.readAll()).object();
    std::vector<EncoderInfo> encoders;
    for (const QJsonValue &v : input.value(QStringLiteral("encoders")).toArray()) encoders.push_back(fromJson(v.toObject()));
    QJsonArray results;
    for (const QJsonValue &v : input.value(QStringLiteral("jobs")).toArray()) {
        const QJsonObject r = toJson(benchJob(encoders, jobFromJson(v.toObject()), std::clamp(seconds, 0.1, 60.0)));
        answer(r, kBenchMark);
        results.append(r);
    }
    answer(results);
    return 0;
}

}  // namespace

bool isHelperCall(int argc, char **argv) {
    return argc > 1 && (std::strcmp(argv[1], "--encoder-probe") == 0 || std::strcmp(argv[1], "--encoder-fit") == 0 ||
                        std::strcmp(argv[1], "--encoder-bench") == 0);
}

int run(int argc, char **argv) {
    if (std::strcmp(argv[1], "--encoder-bench") == 0) return runBench(argc, argv);
    if (std::strcmp(argv[1], "--encoder-probe") == 0) {
        const std::vector<std::string> skip(argv + 2, argv + argc);
        // Each candidate as its test starts and ends, then the whole list.
        answer(toJson(probeEncoders(engine::listGpus(), [](const EncoderInfo &e, bool done) {
            QJsonObject o = toJson(e);
            o.insert(QStringLiteral("done"), done);
            answer(o, kProgress);
        }, [&skip](const EncoderInfo &e) { return std::find(skip.begin(), skip.end(), encoderKey(e)) != skip.end(); })));
        return 0;
    }
    if (argc < 5) return 2;
    const int width = std::atoi(argv[2]), height = std::atoi(argv[3]), fps = std::atoi(argv[4]);
    QFile in;
    if (!in.open(stdin, QIODevice::ReadOnly)) return 2;
    std::vector<EncoderInfo> encoders;
    for (const QJsonValue &v : QJsonDocument::fromJson(in.readAll()).array()) encoders.push_back(fromJson(v.toObject()));
    QJsonArray unfit, slow;
    QJsonObject speeds;
    for (const EncoderInfo &e : encoders) {
        if (!e.available) continue;
        const QString key = QString::fromStdString(encoderKey(e));
        if (!encoderOpensAt(e, width, height, fps)) {
            unfit.append(key);
            continue;
        }
        // Opening is not keeping up: one chip's AV1 block manages half the
        // rate its HEVC block does at 4K. Timed for a moment, hardware only.
        if (!e.hardware || isEditingCodec(e.codec)) continue;
        BenchJob job;
        job.key = encoderKey(e);
        job.width = width;
        job.height = height;
        job.fpsNum = fps;
        job.fpsDen = 1;
        const BenchResult r = benchJob(encoders, job, kFitSeconds);
        if (!r.opened) {
            unfit.append(key);
            continue;
        }
        speeds.insert(key, r.framesPerSecond);
        if (r.framesPerSecond < fps * speedHeadroom(e)) slow.append(key);
    }
    answer(QJsonObject{{QStringLiteral("unfit"), unfit}, {QStringLiteral("slow"), slow}, {QStringLiteral("fps"), speeds}});
    return 0;
}

QString program() {
    return qEnvironmentVariableIsSet("OMABROADCAST_HELPER") ? qEnvironmentVariable("OMABROADCAST_HELPER")
                                                           : QCoreApplication::applicationFilePath();
}

QStringList probeArguments(const QStringList &skip) { return QStringList{QStringLiteral("--encoder-probe")} + skip; }

QStringList fitArguments(int width, int height, int fps) {
    return {QStringLiteral("--encoder-fit"), QString::number(width), QString::number(height), QString::number(fps)};
}

QByteArray fitInput(const std::vector<EncoderInfo> &encoders) {
    return QJsonDocument(toJson(encoders)).toJson(QJsonDocument::Compact);
}

bool readProbe(const QByteArray &output, std::vector<EncoderInfo> *encoders) {
    QJsonDocument doc;
    if (!find(output, &doc) || !doc.isArray()) return false;
    encoders->clear();
    for (const QJsonValue &v : doc.array()) encoders->push_back(fromJson(v.toObject()));
    return !encoders->empty();
}

bool readProgress(const QByteArray &line, EncoderInfo *encoder, bool *done) {
    if (!line.startsWith(kProgress)) return false;
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(line.mid(int(std::strlen(kProgress))), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
    *encoder = fromJson(doc.object());
    *done = doc.object().value(QStringLiteral("done")).toBool();
    return true;
}

bool readFit(const QByteArray &output, Fit *fit) {
    QJsonDocument doc;
    if (!find(output, &doc) || !doc.isObject()) return false;
    const QJsonObject o = doc.object();
    fit->unfit.clear();
    fit->slow.clear();
    fit->fps.clear();
    for (const QJsonValue &v : o.value(QStringLiteral("unfit")).toArray()) fit->unfit.insert(v.toString().toStdString());
    for (const QJsonValue &v : o.value(QStringLiteral("slow")).toArray()) fit->slow.insert(v.toString().toStdString());
    const QJsonObject speeds = o.value(QStringLiteral("fps")).toObject();
    for (auto it = speeds.begin(); it != speeds.end(); ++it) fit->fps[it.key().toStdString()] = it.value().toDouble();
    return true;
}

QStringList benchArguments(double secondsPerJob) {
    return {QStringLiteral("--encoder-bench"), QString::number(secondsPerJob, 'g', 6)};
}

QByteArray benchInput(const std::vector<EncoderInfo> &encoders, const std::vector<BenchJob> &jobs) {
    QJsonArray list;
    for (const BenchJob &j : jobs) list.append(toJson(j));
    return QJsonDocument(QJsonObject{{QStringLiteral("encoders"), toJson(encoders)}, {QStringLiteral("jobs"), list}})
        .toJson(QJsonDocument::Compact);
}

bool readBench(const QByteArray &output, std::vector<BenchResult> *results) {
    QJsonDocument doc;
    if (!find(output, &doc) || !doc.isArray()) return false;
    std::vector<BenchResult> out;
    for (const QJsonValue &v : doc.array()) {
        BenchResult r;
        if (!resultFromJson(v, &r)) return false;
        out.push_back(r);
    }
    *results = std::move(out);
    return true;
}

bool readBenchLine(const QByteArray &line, BenchResult *result) {
    const QByteArray trimmed = line.trimmed();
    const QByteArray mark = QByteArray(kBenchMark).trimmed();
    if (!trimmed.startsWith(mark)) return false;
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.mid(mark.size()).trimmed(), &error);
    return error.error == QJsonParseError::NoError && doc.isObject() && resultFromJson(doc.object(), result);
}

}  // namespace encoderhelper
