// The playback window: the last recording played back to check it, with its
// own picture, sound and meters (see core/recording_player.h).

#include "backend.h"

#include <QDir>
#include <QFileInfo>

void Backend::setPlaybackOpen(bool open) {
    if (m_playbackOpen == open) return;
    // Before the window exists: Hyprland applies a window's rules as it opens.
    if (open && captureHidingAvailable()) m_hyprland.setPlayback(true);
    m_playbackOpen = open;
    if (open) {
        m_player.setTruePeak(m_session.profile().meter.value(QStringLiteral("truePeak"), false).toBool());
        m_player.open(lastRecording());
    } else {
        m_player.close();
    }
    emit playbackChanged();
}

QString Backend::lastRecording() const {
    if (!m_lastRecording.isEmpty() && QFileInfo::exists(m_lastRecording)) return m_lastRecording;
    // From an earlier run: the newest film in the folder, never the one being written.
    const QString writing = m_session.recording() ? QFileInfo(m_session.stats().recordingPath).absoluteFilePath() : QString();
    const QFileInfoList files = QDir(m_session.profile().resolvedFolder())
        .entryInfoList({QStringLiteral("*.mkv"), QStringLiteral("*.mp4"), QStringLiteral("*.mov"), QStringLiteral("*.webm"),
                        QStringLiteral("*.flv"), QStringLiteral("*.ts")}, QDir::Files, QDir::Time);
    for (const QFileInfo &file : files)
        if (file.absoluteFilePath() != writing && file.size() > 0) return file.absoluteFilePath();
    return {};
}

std::string Backend::playbackSoundTarget(QString *why) const {
    const Profile &profile = m_session.profile();
    const std::string fallback = m_session.defaultAudioOutput();
    // Monitoring's output when one is chosen (often headphones), else the desktop's.
    const std::string target = profile.monitorOutput.isEmpty() || profile.monitorOutput == QLatin1String("none")
                                   ? fallback : profile.monitorOutput.toStdString();
    if (target.empty()) {
        *why = tr("No sound output was found.");
        return {};
    }
    // The output's sound is being taken in: playing into it would put the check
    // into the recording, the stream or the next saved replay.
    if (playbackWouldGoOut(target, fallback)) {
        *why = tr("Silent while desktop sound is being recorded, streamed or kept for a replay. Choose a monitoring output in Settings → Audio to hear it.");
        return {};
    }
    return target;
}

bool Backend::playbackWouldGoOut(const std::string &output, const std::string &defaultOutput) const {
    if (output.empty()) return false;
    if (!m_session.recording() && !m_session.streaming() && !m_session.replayActive()) return false;
    const Profile &profile = m_session.profile();
    // Desktop sound hears whatever plays on the desktop's own output.
    if (profile.desktopAdded && !profile.desktopMuted && output == defaultOutput) return true;
    // An output captured by its name, in the mixer for every studio or as a source.
    const auto takes = [&](const SourceDef &source) {
        return source.kind == QLatin1String("audio-output") && !source.settings.value(QStringLiteral("muted")).toBool() &&
               source.settings.value(QStringLiteral("audioNode")).toString().toStdString() == output;
    };
    for (const SourceDef &device : profile.globalAudioDevices)
        if (takes(device)) return true;
    for (const SourceDef &source : m_production.collection().sources)
        if (takes(source)) return true;
    return false;
}
