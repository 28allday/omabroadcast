#include "recorder.h"

#include <time.h>

#include <filesystem>

Recorder::~Recorder() { stop(); }

std::string Recorder::expandTemplate(const std::string &tmpl, int64_t unixSeconds) {
    const time_t t = unixSeconds;
    tm local{};
    localtime_r(&t, &local);
    std::string out;
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != '%' || i + 1 >= tmpl.size()) { out += tmpl[i]; continue; }
        const char c = tmpl[++i];
        char buf[8];
        auto two = [&](int v) { snprintf(buf, sizeof buf, "%02d", v); out += buf; };
        switch (c) {
        case 'Y': snprintf(buf, sizeof buf, "%04d", local.tm_year + 1900); out += buf; break;
        case 'y': two(local.tm_year % 100); break;
        case 'm': two(local.tm_mon + 1); break;
        case 'd': two(local.tm_mday); break;
        case 'H': two(local.tm_hour); break;
        case 'M': two(local.tm_min); break;
        case 'S': two(local.tm_sec); break;
        case '%': out += '%'; break;
        default: out += '%'; out += c; break;
        }
    }
    return out;
}

std::string Recorder::formatForPath(const std::string &path) {
    const std::string ext = std::filesystem::path(path).extension().string();
    for (const FileFormat &f : fileFormats())
        if (ext == std::string(".") + f.extension && !f.movflags) return f.id;
    return "mkv";
}

bool Recorder::start(const Settings &settings, std::string *error) {
    stop();
    EncodedStream::Settings s;
    s.encoder = settings.encoder;
    s.width = settings.width;
    s.height = settings.height;
    s.fpsNum = settings.fpsNum;
    s.fpsDen = settings.fpsDen;
    s.colour = settings.colour;
    s.video.mode = settings.bitrateKbps > 0 ? VideoRate::Mode::Cbr : VideoRate::Mode::Quality;
    s.video.quality = settings.quality;
    s.video.bitrateKbps = settings.bitrateKbps;
    s.audioTracks = settings.audio ? settings.audioTracks & 63 : 0;
    s.audioBitrateKbps = settings.audioBitrateKbps;
    s.sampleRate = settings.sampleRate;
    s.channels = settings.channels;
    s.startTimeNs = settings.startTimeNs;
    m_stream.onError = [this](const std::string &message) { if (onError) onError(message); };
    m_stream.onFrameDone = [this](engine::YuvFrame &&frame) { if (onFrameDone) onFrameDone(std::move(frame)); };
    if (!m_stream.start(s, error)) return false;

    FileSink::Settings f;
    f.path = settings.path;
    f.format = settings.format.empty() ? formatForPath(settings.path) : settings.format;
    m_file = std::make_shared<FileSink>();
    m_file->onError = [this](const std::string &message) { if (onError) onError(message); };
    if (!m_file->start(f, m_stream.layout(), error)) {
        m_file.reset();
        m_stream.stop();
        return false;
    }
    m_stream.attach(m_file);
    return true;
}

void Recorder::stop() {
    m_stream.stop();  // flushes, then ends the file
    if (m_file) m_file->join();
}

Recorder::Stats Recorder::stats() const {
    const EncodedStream::Stats e = m_stream.stats();
    Stats s;
    s.framesEncoded = e.framesEncoded;
    s.framesDropped = e.framesDropped;
    s.durationNs = e.durationNs;
    s.encodeMs = e.encodeMs;
    if (m_file) s.bytesWritten = m_file->stats().bytes;
    return s;
}
