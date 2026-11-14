#include "backend.h"

#include <QGuiApplication>
#include <QFileInfo>
#include <QRegularExpression>

// Tools: the automatic studio switcher and the output timer.

void Backend::setSwitcherEnabled(bool on) {
    QVariantMap s = m_production.collection().switcher;
    s.insert(QStringLiteral("enabled"), on);
    m_production.setSwitcher(s);
}

void Backend::setSwitcherRules(const QVariantList &rules) {
    QVariantList clean;
    for (const QVariant &r : rules) {
        const QVariantMap rule = r.toMap();
        clean.append(QVariantMap{{"field", rule.value("field").toString() == QLatin1String("title") ? QStringLiteral("title") : QStringLiteral("class")},
                                 {"pattern", rule.value("pattern").toString()}, {"studio", rule.value("studio").toString()}});
    }
    QVariantMap s = m_production.collection().switcher;
    s.insert(QStringLiteral("rules"), clean);
    m_production.setSwitcher(s);
}

void Backend::setSwitcherOtherwise(const QString &studio) {
    QVariantMap s = m_production.collection().switcher;
    s.insert(QStringLiteral("otherwise"), studio);
    m_production.setSwitcher(s);
}

QString Backend::focusedWindow() const {
    if (m_focusedClass.isEmpty() && m_focusedTitle.isEmpty()) return {};
    return m_focusedTitle.isEmpty() ? m_focusedClass : tr("%1 — %2").arg(m_focusedClass, m_focusedTitle);
}

void Backend::syncSwitcher() {
    if (switcherEnabled() && !safeMode()) m_focus.start();
    else m_focus.stop();
}

void Backend::windowFocused(const QString &windowClass, const QString &title) {
    m_focusedClass = windowClass;
    m_focusedTitle = title;
    emit focusedWindowChanged();
    if (!switcherEnabled()) return;
    // Our own windows (going to change a rule) change nothing.
    if (windowClass == QGuiApplication::desktopFileName()) return;
    const Collection &c = m_production.collection();
    QString studio;
    for (const QVariant &r : switcherRules()) {
        const QVariantMap rule = r.toMap();
        const QRegularExpression pattern(rule.value("pattern").toString(), QRegularExpression::CaseInsensitiveOption);
        if (rule.value("pattern").toString().isEmpty() || !pattern.isValid()) continue;
        if (pattern.match(rule.value("field").toString() == QLatin1String("title") ? title : windowClass).hasMatch()) {
            studio = rule.value("studio").toString();
            break;
        }
    }
    if (studio.isEmpty()) studio = switcherOtherwise();
    const Scene *scene = c.scene(studio);
    if (!scene || scene->group || studio == programScene()) return;
    // The program changes; in studio mode the studio being edited stays.
    if (studioMode()) m_session.takeProgram(studio);
    else setCurrentScene(studio);
}

void Backend::setRecordTimerSeconds(int seconds) {
    setPreference(QStringLiteral("recordTimerSeconds"), std::clamp(seconds, 0, 7 * 24 * 3600));
    emit timersChanged();
}

void Backend::setStreamTimerSeconds(int seconds) {
    setPreference(QStringLiteral("streamTimerSeconds"), std::clamp(seconds, 0, 7 * 24 * 3600));
    emit timersChanged();
}

int Backend::recordTimerLeft() const {
    return m_recordElapsed < 0 || recordTimerSeconds() <= 0 ? -1 : std::max(0, recordTimerSeconds() - m_recordElapsed);
}

int Backend::streamTimerLeft() const {
    return m_streamElapsed < 0 || streamTimerSeconds() <= 0 ? -1 : std::max(0, streamTimerSeconds() - m_streamElapsed);
}

void Backend::tickOutputTimer() {
    // Counted while it runs; a paused recording's pause does not count.
    const bool rec = recording() && !recordingStopping(), live = streaming() && !m_session.streamingStopping();
    m_recordElapsed = rec ? std::max(0, m_recordElapsed) + (m_session.recordingPaused() ? 0 : 1) : -1;
    m_streamElapsed = live ? std::max(0, m_streamElapsed) + 1 : -1;
    if (rec && recordTimerSeconds() > 0 && m_recordElapsed >= recordTimerSeconds()) {
        toggleRecording();
        setStatus(tr("The output timer stopped the recording."));
        m_recordElapsed = -1;
    }
    if (live && streamTimerSeconds() > 0 && m_streamElapsed >= streamTimerSeconds()) {
        toggleStreaming();
        setStatus(tr("The output timer ended the stream."));
        m_streamElapsed = -1;
    }
    emit timersChanged();
}

QVariantList Backend::scripts() const {
    QVariantList out;
    for (const QVariant &v : preference(QStringLiteral("scripts"), QVariantList()).toList()) {
        const QVariantMap s = v.toMap();
        const QString path = s.value("path").toString();
        out.append(QVariantMap{{"path", path}, {"name", QFileInfo(path).fileName()}, {"enabled", s.value("enabled", true).toBool()},
                               {"running", m_scripts.running(path)}});
    }
    return out;
}

void Backend::addScript() {
    m_otherChosen = [this](const QString &path) { addScriptFile(path); };
    m_otherPicker.openFile(tr("Add a script"), tr("Scripts"), {QStringLiteral("*.js")});
}

void Backend::addScriptFile(const QString &path) {
    QVariantList list = preference(QStringLiteral("scripts"), QVariantList()).toList();
    for (const QVariant &v : list) if (v.toMap().value("path").toString() == path) return;
    list.append(QVariantMap{{"path", path}, {"enabled", true}});
    setPreference(QStringLiteral("scripts"), list);
    if (!safeMode()) m_scripts.setScripts(list);  // safe mode runs none, however the list changes
    emit scriptsChanged();
}

void Backend::removeScript(const QString &path) {
    QVariantList list = preference(QStringLiteral("scripts"), QVariantList()).toList();
    list.removeIf([&](const QVariant &v) { return v.toMap().value("path").toString() == path; });
    setPreference(QStringLiteral("scripts"), list);
    if (!safeMode()) m_scripts.setScripts(list);  // safe mode runs none, however the list changes
    emit scriptsChanged();
}

void Backend::setScriptEnabled(const QString &path, bool enabled) {
    QVariantList list = preference(QStringLiteral("scripts"), QVariantList()).toList();
    for (QVariant &v : list) {
        QVariantMap s = v.toMap();
        if (s.value("path").toString() == path) { s.insert("enabled", enabled); v = s; }
    }
    setPreference(QStringLiteral("scripts"), list);
    if (!safeMode()) m_scripts.setScripts(list);  // safe mode runs none, however the list changes
    emit scriptsChanged();
}

// What scripts can follow with omabroadcast.on(...).
void Backend::sendScriptEvents() {
    auto studioEvent = [this](const QString &id) {
        const Scene *s = m_production.collection().scene(id);
        return QVariantMap{{"id", id}, {"name", s ? s->name : QString()}};
    };
    connect(&m_production, &Production::changed, this, [this, studioEvent] {
        if (currentScene() == m_scriptStudio) return;
        m_scriptStudio = currentScene();
        m_scripts.send(QStringLiteral("studio"), studioEvent(m_scriptStudio));
    });
    auto program = [this, studioEvent] {
        if (transitioning() || programScene() == m_scriptProgram) return;
        m_scriptProgram = programScene();
        m_scripts.send(QStringLiteral("program"), studioEvent(m_scriptProgram));
    };
    connect(&m_session, &Session::transitionChanged, this, program);
    connect(&m_session, &Session::studioModeChanged, this, program);
    connect(&m_production, &Production::changed, this, program);
    connect(this, &Backend::recordingChanged, this, [this] { m_scripts.send(QStringLiteral("recording"), {{"recording", recording()}}); });
    connect(this, &Backend::streamingChanged, this, [this] { m_scripts.send(QStringLiteral("streaming"), {{"streaming", streaming()}}); });
    connect(&m_session, &Session::replaySaved, this, [this](const QString &path) { m_scripts.send(QStringLiteral("replaySaved"), {{"path", path}}); });
    connect(this, &Backend::screenshotSaved, this, [this](const QString &path) { m_scripts.send(QStringLiteral("screenshot"), {{"path", path}}); });
    connect(this, &Backend::focusedWindowChanged, this, [this] {
        m_scripts.send(QStringLiteral("focus"), {{"class", m_focusedClass}, {"title", m_focusedTitle}});
    });
}

QVariantMap Backend::scriptValues(const QString &path) const {
    return preference(QStringLiteral("scriptSettings"), QVariantMap()).toMap().value(path).toMap();
}

QVariantList Backend::scriptForm(const QString &path) const {
    QVariantList out;
    const QVariantMap form = m_scriptForms.value(path), values = scriptValues(path);
    for (auto it = form.begin(); it != form.end(); ++it) {
        const QVariantMap field = it.value().toMap();
        const QString type = field.value("type").toString();
        out.append(QVariantMap{{"key", it.key()}, {"label", field.value("label", it.key())},
                               {"type", QStringList{"number", "bool", "studio"}.contains(type) ? type : QStringLiteral("text")},
                               {"value", values.contains(it.key()) ? values.value(it.key()) : field.value("default")}});
    }
    return out;
}

void Backend::setScriptSetting(const QString &path, const QString &key, const QVariant &value) {
    QVariantMap all = preference(QStringLiteral("scriptSettings"), QVariantMap()).toMap();
    QVariantMap mine = all.value(path).toMap();
    mine.insert(key, value);
    all.insert(path, mine);
    setPreference(QStringLiteral("scriptSettings"), all);
    QVariantMap values;
    for (const QVariant &f : scriptForm(path)) values.insert(f.toMap().value("key").toString(), f.toMap().value("value"));
    m_scripts.settingsChanged(path, values);
    emit scriptsChanged();
}
