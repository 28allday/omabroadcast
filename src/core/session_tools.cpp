// What the stats dock shows of the outputs, kept apart from the session's
// frame and output code.
#include "session.h"

static QString encoderOf(const EncodedStream &stream) { return QString::fromStdString(stream.settings().encoder.label); }

std::vector<Session::OutputStats> Session::outputStats() const {
    std::vector<OutputStats> out;
    if (m_recording) {
        const EncodedStream::Stats e = m_recording->stream->stats();
        out.push_back({QStringLiteral("recording"), {}, e.encodeMs, encoderOf(*m_recording->stream), m_recording->stream->settings().encoder.hardware, -1, e.framesEncoded, e.framesDropped});
    }
    const std::vector<StreamStatus> streams = streamStatus();
    for (size_t i = 0; i < m_streams.size() && i < streams.size(); ++i) {
        const EncodedStream::Stats e = m_streams[i]->stream->stats();
        out.push_back({QStringLiteral("stream"), streams[i].name, e.encodeMs, encoderOf(*m_streams[i]->stream), m_streams[i]->stream->settings().encoder.hardware,
                       streams[i].lagMs, streams[i].framesSent, streams[i].framesDropped});
    }
    if (m_replay) {
        const EncodedStream::Stats e = m_replay->stream->stats();
        out.push_back({QStringLiteral("replay"), {}, e.encodeMs, encoderOf(*m_replay->stream), m_replay->stream->settings().encoder.hardware, -1, e.framesEncoded, e.framesDropped});
    }
    if (m_camera.running())
        out.push_back({QStringLiteral("camera"), {}, -1, {}, false, -1, m_camera.framesSent(), m_camera.framesDropped()});
    return out;
}

QString Session::engineRenderNode() const {
    std::lock_guard<std::mutex> lock(m_encoderMutex);
    return QString::fromStdString(m_engineDevice.renderNode);
}
