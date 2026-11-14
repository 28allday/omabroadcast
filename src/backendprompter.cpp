#include "backend.h"

#include "core/prompter_script.h"

#include <algorithm>

// The prompter: a script in a window of its own, for the person on camera. It is
// drawn only by that window and never reaches the engine, so no recording, stream
// or virtual camera can hold it. A capture of the screen could, so on Hyprland the
// window is kept out of those too.

void Backend::setPrompterOpen(bool open) {
    if (m_prompterOpen == open) return;
    // Before the window exists: Hyprland applies a window's rules as it opens.
    if (open) m_prompterHidden = captureHidingAvailable() && m_hyprland.setPrompter(true);
    m_prompterOpen = open;
    if (open && m_prompterText.isEmpty()) {
        const QString saved = prompterFile();
        if (!saved.isEmpty()) loadPrompter(saved);
    }
    if (!open) m_prompterPlaying = false;
    emit prompterChanged();
}

void Backend::setPrompterPlaying(bool playing) {
    playing = playing && !m_prompterText.isEmpty();
    if (m_prompterPlaying == playing) return;
    m_prompterPlaying = playing;
    emit prompterChanged();
}

void Backend::setPrompterSpeed(int speed) {
    setPreference(QStringLiteral("prompterSpeed"), std::clamp(speed, 1, 40));
}

void Backend::setPrompterSize(int size) {
    setPreference(QStringLiteral("prompterSize"), std::clamp(size, 16, 160));
}

void Backend::setPrompterWindowSize(int width, int height) {
    setPreference(QStringLiteral("prompterWidth"), width);
    setPreference(QStringLiteral("prompterHeight"), height);
}

void Backend::restartPrompter() {
    emit prompterRestarted();
}

bool Backend::loadPrompter(const QString &path) {
    QString error;
    const QString text = prompter::readScript(path, &error);
    if (text.isEmpty()) {
        m_prompterProblem = error;
        emit prompterChanged();
        return false;
    }
    m_prompterText = text;
    m_prompterProblem.clear();
    m_prompterPlaying = false;
    setPreference(QStringLiteral("prompterFile"), path);
    emit prompterChanged();
    emit prompterRestarted();
    return true;
}

void Backend::choosePrompterFile() {
    m_otherChosen = [this](const QString &path) { if (!path.isEmpty()) loadPrompter(path); };
    m_otherPicker.openFile(tr("Load a script"), tr("Scripts"),
                           {QStringLiteral("*.txt"), QStringLiteral("*.text"), QStringLiteral("*.md"), QStringLiteral("*.markdown")});
}
