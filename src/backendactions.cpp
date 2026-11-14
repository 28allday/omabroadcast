#include "backend.h"

#include "core/transitions.h"
#include "core/video_filter_settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QKeySequence>
#include <QWindow>
#include <QJsonArray>

// Every action people can bind to a key, run with `omabroadcast ctl` or ask
// for over the control socket. Actions taking an argument name a studio, an
// item, a source, an audio channel, a transition or a filter, by id or name.

namespace {

struct ActionDef {
    const char *id, *group, *argument, *name;
};

// argument: "" none; studio; item (in the current studio); media; slideshow;
// channel (desktop, microphone or a source with sound); transition; quick
// (1-based); filter ("source/filter"); text (anything, optional).
const ActionDef kActions[] = {
    {"record.toggle", "Recording", "", QT_TRANSLATE_NOOP("Actions", "Start or stop recording")},
    {"record.start", "Recording", "", QT_TRANSLATE_NOOP("Actions", "Start recording")},
    {"record.stop", "Recording", "", QT_TRANSLATE_NOOP("Actions", "Stop recording")},
    {"record.pause", "Recording", "", QT_TRANSLATE_NOOP("Actions", "Pause or resume recording")},
    {"record.split", "Recording", "", QT_TRANSLATE_NOOP("Actions", "Start a new recording file")},
    {"record.chapter", "Recording", "text", QT_TRANSLATE_NOOP("Actions", "Mark a chapter")},
    {"stream.toggle", "Streaming", "", QT_TRANSLATE_NOOP("Actions", "Go live or end the stream")},
    {"stream.start", "Streaming", "", QT_TRANSLATE_NOOP("Actions", "Go live")},
    {"stream.stop", "Streaming", "", QT_TRANSLATE_NOOP("Actions", "End the stream")},
    {"replay.toggle", "Replay", "", QT_TRANSLATE_NOOP("Actions", "Start or stop the replay buffer")},
    {"replay.save", "Replay", "", QT_TRANSLATE_NOOP("Actions", "Save replay")},
    {"camera.toggle", "Virtual camera", "", QT_TRANSLATE_NOOP("Actions", "Start or stop the virtual camera")},
    {"screenshot.program", "Screenshots", "", QT_TRANSLATE_NOOP("Actions", "Screenshot of the program")},
    {"screenshot.preview", "Screenshots", "", QT_TRANSLATE_NOOP("Actions", "Screenshot of the preview")},
    {"screenshot.studio", "Screenshots", "studio", QT_TRANSLATE_NOOP("Actions", "Screenshot of %1")},
    {"prompter.toggle", "Prompter", "", QT_TRANSLATE_NOOP("Actions", "Open or close the prompter")},
    {"prompter.play", "Prompter", "", QT_TRANSLATE_NOOP("Actions", "Start or pause the prompter")},
    {"prompter.restart", "Prompter", "", QT_TRANSLATE_NOOP("Actions", "Take the prompter back to the start")},
    {"prompter.faster", "Prompter", "", QT_TRANSLATE_NOOP("Actions", "Make the prompter faster")},
    {"prompter.slower", "Prompter", "", QT_TRANSLATE_NOOP("Actions", "Make the prompter slower")},
    {"studiomode.toggle", "Studio mode", "", QT_TRANSLATE_NOOP("Actions", "Turn studio mode on or off")},
    {"take", "Studio mode", "", QT_TRANSLATE_NOOP("Actions", "Take the preview to the program")},
    {"transition.quick", "Studio mode", "quick", QT_TRANSLATE_NOOP("Actions", "Take with quick transition %1")},
    {"studio.show", "Studios", "studio", QT_TRANSLATE_NOOP("Actions", "Show %1")},
    {"studio.next", "Studios", "", QT_TRANSLATE_NOOP("Actions", "Show the next studio")},
    {"studio.previous", "Studios", "", QT_TRANSLATE_NOOP("Actions", "Show the previous studio")},
    {"transition.choose", "Transitions", "transition", QT_TRANSLATE_NOOP("Actions", "Choose the transition %1")},
    {"item.toggle", "Sources", "item", QT_TRANSLATE_NOOP("Actions", "Show or hide %1")},
    {"item.show", "Sources", "item", QT_TRANSLATE_NOOP("Actions", "Show %1")},
    {"item.hide", "Sources", "item", QT_TRANSLATE_NOOP("Actions", "Hide %1")},
    {"media.playpause", "Media", "media", QT_TRANSLATE_NOOP("Actions", "Play or pause %1")},
    {"media.restart", "Media", "media", QT_TRANSLATE_NOOP("Actions", "Restart %1")},
    {"media.stop", "Media", "media", QT_TRANSLATE_NOOP("Actions", "Stop %1")},
    {"slideshow.next", "Slideshows", "slideshow", QT_TRANSLATE_NOOP("Actions", "Next slide in %1")},
    {"slideshow.previous", "Slideshows", "slideshow", QT_TRANSLATE_NOOP("Actions", "Previous slide in %1")},
    {"slideshow.playpause", "Slideshows", "slideshow", QT_TRANSLATE_NOOP("Actions", "Play or pause %1")},
    {"audio.mute", "Audio", "channel", QT_TRANSLATE_NOOP("Actions", "Mute or unmute %1")},
    {"filter.toggle", "Filters", "filter", QT_TRANSLATE_NOOP("Actions", "Switch %1 on or off")},
};

QString trAction(const char *text) { return QCoreApplication::translate("Actions", text); }

const ActionDef *findAction(const QString &id) {
    for (const ActionDef &a : kActions)
        if (id == QLatin1String(a.id)) return &a;
    return nullptr;
}

bool sameName(const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) == 0; }

// A filter's name as people see it: its kind's label.
QString filterLabel(const QString &type) {
    for (const QVariant &d : VideoFilterSettings::catalog())
        if (d.toMap().value("type").toString() == type) return d.toMap().value("label").toString();
    return type;
}

}  // namespace

QVariantList Backend::actions() const {
    const Collection &c = m_production.collection();
    QVariantList out;
    auto add = [&](const ActionDef &a, const QString &argument, const QString &label) {
        out.append(QVariantMap{{"id", QString(a.id)}, {"argument", argument}, {"group", trAction(a.group)},
                               {"name", label.isEmpty() ? trAction(a.name) : trAction(a.name).arg(label)}, {"global", QString(a.argument).isEmpty()}});
    };
    for (const ActionDef &a : kActions) {
        const QString kind = a.argument;
        if (kind.isEmpty() || kind == QLatin1String("text")) add(a, {}, {});
        else if (kind == QLatin1String("studio")) {
            for (const Scene &s : c.scenes) if (!s.group) add(a, s.id, s.name);
        } else if (kind == QLatin1String("item")) {
            for (const Scene &s : c.scenes) {
                if (s.group) continue;
                for (const SceneItem &i : s.items)
                    if (const SourceDef *src = c.source(i.source)) add(a, i.id, tr("%1 in %2").arg(src->name, s.name));
            }
        } else if (kind == QLatin1String("media") || kind == QLatin1String("slideshow")) {
            for (const SourceDef &s : c.sources) if (s.kind == kind) add(a, s.id, s.name);
        } else if (kind == QLatin1String("channel")) {
            if (desktopAdded()) add(a, QStringLiteral("desktop"), tr("desktop audio"));
            if (microphoneAdded()) add(a, QStringLiteral("microphone"), tr("the microphone"));
            for (const SourceDef &s : c.sources) if (s.hasAudio()) add(a, s.id, s.name);
        } else if (kind == QLatin1String("transition")) {
            for (const QVariant &t : Transitions::all(c)) add(a, t.toMap().value("id").toString(), t.toMap().value("name").toString());
        } else if (kind == QLatin1String("quick")) {
            for (int i = 0; i < c.quickTransitions.size(); ++i) add(a, QString::number(i + 1), QString::number(i + 1));
        } else if (kind == QLatin1String("filter")) {
            for (const SourceDef &s : c.sources)
                for (const QVariant &f : s.settings.value(QStringLiteral("videoFilters")).toList()) {
                    const QVariantMap filter = f.toMap();
                    add(a, s.id + QLatin1Char('/') + filter.value("id").toString(), tr("%1 on %2").arg(filterLabel(filter.value("type").toString()), s.name));
                }
        }
    }
    return out;
}

QString Backend::runAction(const QString &action, const QString &argument) {
    const ActionDef *def = findAction(action);
    if (!def) return tr("There is no action called \"%1\".").arg(action);
    const Collection &c = m_production.collection();
    const QString kind = def->argument;
    if (!kind.isEmpty() && kind != QLatin1String("text") && argument.isEmpty()) return tr("\"%1\" needs to know which %2.").arg(action, kind);
    auto studio = [&]() -> const Scene * {
        for (const Scene &s : c.scenes) if (!s.group && (s.id == argument || sameName(s.name, argument))) return &s;
        return nullptr;
    };
    auto source = [&](const QString &want, const QString &kindWanted) -> const SourceDef * {
        for (const SourceDef &s : c.sources)
            if ((kindWanted.isEmpty() || s.kind == kindWanted) && (s.id == want || sameName(s.name, want))) return &s;
        return nullptr;
    };
    const QString id = action;
    // What needs an encoder cannot start until they are known: said, rather
    // than an answer that looks like it did.
    const QString stillChecking = m_session.probing() ? m_session.stillChecking() : QString();
    const bool startingRecording = !recording() && !m_session.recordingStopping();
    if (id == QLatin1String("record.toggle")) {
        if (!startingRecording) toggleRecording();
        else if (const QString why = startRecording(); !why.isEmpty()) return why;
    } else if (id == QLatin1String("record.start")) {
        if (startingRecording)
            if (const QString why = startRecording(); !why.isEmpty()) return why;
    }
    else if (id == QLatin1String("record.stop")) { if (recording()) toggleRecording(); }
    else if (id == QLatin1String("record.pause")) {
        if (!recording()) return tr("Not recording.");
        setRecordingPaused(!m_session.recordingPaused());
    } else if (id == QLatin1String("record.split")) {
        if (!recording()) return tr("Not recording.");
        splitRecording();
    } else if (id == QLatin1String("record.chapter")) {
        if (!recording()) return tr("Not recording.");
        addChapter(argument);
    } else if (id == QLatin1String("stream.toggle") || id == QLatin1String("stream.start")) {
        if (!streaming() && !stillChecking.isEmpty()) return stillChecking;
        if (id == QLatin1String("stream.toggle") || !streaming()) toggleStreaming();
    }
    else if (id == QLatin1String("stream.stop")) { if (streaming()) toggleStreaming(); }
    else if (id == QLatin1String("replay.toggle")) {
        if (!replayActive() && !stillChecking.isEmpty()) return stillChecking;
        toggleReplay();
    }
    else if (id == QLatin1String("replay.save")) {
        if (!replayActive()) return tr("The replay buffer is not running.");
        saveReplay();
    } else if (id == QLatin1String("camera.toggle")) toggleVirtualCamera();
    else if (id == QLatin1String("screenshot.program")) screenshot(QStringLiteral("program"));
    else if (id == QLatin1String("screenshot.preview")) screenshot(QStringLiteral("preview"));
    else if (id == QLatin1String("screenshot.studio")) {
        const Scene *s = studio();
        if (!s) return tr("There is no studio called \"%1\".").arg(argument);
        screenshot(QStringLiteral("studio"), s->id);
    } else if (id == QLatin1String("prompter.toggle")) setPrompterOpen(!prompterOpen());
    else if (id == QLatin1String("prompter.play")) setPrompterPlaying(!prompterPlaying());
    else if (id == QLatin1String("prompter.restart")) restartPrompter();
    else if (id == QLatin1String("prompter.faster")) setPrompterSpeed(prompterSpeed() + 1);
    else if (id == QLatin1String("prompter.slower")) setPrompterSpeed(prompterSpeed() - 1);
    else if (id == QLatin1String("studiomode.toggle")) setStudioMode(!studioMode());
    else if (id == QLatin1String("take")) {
        if (!studioMode()) return tr("Studio mode is off.");
        take();
    } else if (id == QLatin1String("transition.quick")) {
        if (!studioMode()) return tr("Studio mode is off.");
        const int index = argument.toInt() - 1;
        if (index < 0 || index >= c.quickTransitions.size()) return tr("There is no quick transition %1.").arg(argument);
        quickTake(index);
    } else if (id == QLatin1String("studio.show")) {
        const Scene *s = studio();
        if (!s) return tr("There is no studio called \"%1\".").arg(argument);
        setCurrentScene(s->id);
    } else if (id == QLatin1String("studio.next") || id == QLatin1String("studio.previous")) {
        QStringList ids;
        for (const Scene &s : c.scenes) if (!s.group) ids << s.id;
        if (ids.isEmpty()) return tr("There are no studios.");
        const int at = std::max<int>(0, ids.indexOf(c.currentScene)), step = id == QLatin1String("studio.next") ? 1 : -1;
        setCurrentScene(ids[(at + step + ids.size()) % ids.size()]);
    } else if (id == QLatin1String("transition.choose")) {
        QString found;
        for (const QVariant &t : Transitions::all(c))
            if (t.toMap().value("id").toString() == argument || sameName(t.toMap().value("name").toString(), argument)) found = t.toMap().value("id").toString();
        if (found.isEmpty()) return tr("There is no transition called \"%1\".").arg(argument);
        setCurrentTransition(found);
    } else if (id.startsWith(QLatin1String("item."))) {
        // By id anywhere, or by its source's name in the current studio.
        const SceneItem *item = c.findItem(argument);
        if (!item && c.current())
            for (const SceneItem &i : c.current()->items)
                if (const SourceDef *s = c.source(i.source); s && sameName(s->name, argument)) { item = &i; break; }
        if (!item) return tr("There is no source called \"%1\" in this studio.").arg(argument);
        const bool visible = id == QLatin1String("item.toggle") ? !item->visible : id == QLatin1String("item.show");
        setItemVisible(item->id, visible);
    } else if (id.startsWith(QLatin1String("media."))) {
        const SourceDef *s = source(argument, QStringLiteral("media"));
        if (!s) return tr("There is no media source called \"%1\".").arg(argument);
        if (id == QLatin1String("media.playpause")) {
            const QVariantMap playback = m_session.mediaPlayback(s->id);
            if (playback.value("state").toString() == QLatin1String("ended")) controlMedia(s->id, QStringLiteral("restart"));
            else controlMedia(s->id, QStringLiteral("pause"), playback.value("paused").toBool() ? 0 : 1);
        } else controlMedia(s->id, id.mid(6));
    } else if (id.startsWith(QLatin1String("slideshow."))) {
        const SourceDef *s = source(argument, QStringLiteral("slideshow"));
        if (!s) return tr("There is no slideshow called \"%1\".").arg(argument);
        if (id == QLatin1String("slideshow.playpause"))
            controlSlideshow(s->id, QStringLiteral("pause"), m_session.slideshowPlayback(s->id).value("paused").toBool() ? 0 : 1);
        else controlSlideshow(s->id, id.mid(10));
    } else if (id == QLatin1String("audio.mute")) {
        if (argument == QLatin1String("desktop")) setDesktopMuted(!desktopMuted());
        else if (argument == QLatin1String("microphone")) setMicrophoneMuted(!microphoneMuted());
        else {
            const SourceDef *s = source(argument, {});
            if (!s || !s->hasAudio()) return tr("There is no sound called \"%1\".").arg(argument);
            setSourceSetting(s->id, QStringLiteral("muted"), !s->settings.value(QStringLiteral("muted"), false).toBool());
        }
    } else if (id == QLatin1String("filter.toggle")) {
        const int slash = argument.lastIndexOf(QLatin1Char('/'));
        const SourceDef *s = slash > 0 ? source(argument.left(slash), {}) : nullptr;
        if (!s) return tr("Name a filter as source/filter.");
        QVariantList filters = s->settings.value(QStringLiteral("videoFilters")).toList();
        const QString want = argument.mid(slash + 1);
        bool found = false;
        for (QVariant &f : filters) {
            QVariantMap filter = f.toMap();
            if (filter.value("id").toString() != want && !sameName(filterLabel(filter.value("type").toString()), want) && !sameName(filter.value("type").toString(), want)) continue;
            filter.insert(QStringLiteral("enabled"), !filter.value(QStringLiteral("enabled"), true).toBool());
            f = filter;
            found = true;
            break;
        }
        if (!found) return tr("%1 has no filter called \"%2\".").arg(s->name, want);
        setVideoFilters(s->id, filters);
    }
    return {};
}

// The main window (projectors are plain windows).
static QWindow *mainWindow() {
    for (QWindow *w : QGuiApplication::topLevelWindows())
        if (w->inherits("QQuickApplicationWindow")) return w;
    return nullptr;
}

QVariantMap Backend::state() const {
    const Collection &c = m_production.collection();
    auto studioName = [&](const QString &id) { const Scene *s = c.scene(id); return s ? s->name : QString(); };
    return {{"checkingEncoders", m_session.probing()}, {"recording", recording()}, {"recordingPaused", m_session.recordingPaused()}, {"streaming", streaming()},
            {"replay", replayActive()}, {"virtualCamera", virtualCamera()}, {"studioMode", studioMode()},
            {"studio", c.currentScene}, {"studioName", studioName(c.currentScene)},
            {"program", programScene()}, {"programName", studioName(programScene())},
            {"transitioning", transitioning()}, {"transition", c.currentTransition}, {"transitionMs", c.transitionMs},
            {"collection", collectionName()}, {"profile", profileName()},
            {"recordingStopping", recordingStopping()}, {"recordingTime", recordingTime()},
            {"streamingStopping", streamingStopping()}, {"streamingTime", streamingTime()},
            {"windowVisible", mainWindow() && mainWindow()->isVisible()}};
}

QJsonObject Backend::controlRequest(const QJsonObject &request) {
    const QString action = request.value("action").toString(), argument = request.value("argument").toString();
    if (action == QLatin1String("list")) return {{"ok", true}, {"result", QJsonArray::fromVariantList(actions())}};
    if (action == QLatin1String("status")) return {{"ok", true}, {"result", QJsonObject::fromVariantMap(state())}};
    // A second copy started: this one comes forward instead.
    if (action == QLatin1String("window.raise")) {
        if (QWindow *w = mainWindow()) {
            w->show();
            w->raise();
            w->requestActivate();
        }
        return {{"ok", true}};
    }
    // The bar icon: for the window to act on as on its own menus, so a stop that
    // asks first asks here too. Not hotkeys or a script's business, so not listed.
    if (action.startsWith(QLatin1String("bar."))) {
        // The launch card is still up: there is no window to act on it yet.
        if (!m_started && !m_crashOffered) return {{"ok", false}, {"error", tr("OmaBroadcast is still starting.")}, {"retry", true}};
        static const QStringList known{QStringLiteral("window"), QStringLiteral("record"), QStringLiteral("stream"), QStringLiteral("replay"),
                                       QStringLiteral("replaySave"), QStringLiteral("camera"), QStringLiteral("quit")};
        const QString name = action.mid(4);
        if (!known.contains(name)) return {{"ok", false}, {"error", tr("There is no action called \"%1\".").arg(action)}};
        emit barCommand(name);
        return {{"ok", true}};
    }
    // A file named on a second launch, an absolute path; from here only, as it can
    // import a profile. A hotkey or a script has no use for it.
    if (action == QLatin1String("file.open")) {
        if (QDir::isRelativePath(argument)) return {{"ok", false}, {"error", tr("Name the file by its full path.")}};
        open(QUrl::fromLocalFile(argument));
        return {{"ok", true}};
    }
    // Answering from before the window asks for anything: the crash sheet is up
    // and nothing is started.
    if (m_crashOffered) return {{"ok", false}, {"error", tr("OmaBroadcast is waiting for an answer to its crash sheet.")}, {"retry", true}};
    // The same from behind the launch card: no studios are open to act on.
    if (!m_started) return {{"ok", false}, {"error", tr("OmaBroadcast is still starting.")}, {"retry", true}};
    const QString error = runAction(action, argument);
    if (!error.isEmpty()) {
        // Worth asking again shortly, which a script can tell from a real no.
        QJsonObject refused{{"ok", false}, {"error", error}};
        if (error == m_session.stillChecking()) refused.insert("retry", true);
        return refused;
    }
    return {{"ok", true}, {"result", QJsonObject::fromVariantMap(state())}};
}

QVariantMap Backend::hotkeys() const {
    QVariantMap out = m_production.collection().hotkeys;
    for (auto it = m_session.profile().hotkeys.begin(); it != m_session.profile().hotkeys.end(); ++it) out.insert(it.key() + QLatin1Char('|'), it.value());
    return out;
}

void Backend::setHotkey(const QString &action, const QString &argument, const QString &key) {
    const ActionDef *def = findAction(action);
    if (!def) return;
    const QString text = QKeySequence::fromString(key, QKeySequence::PortableText).toString(QKeySequence::PortableText);
    if (QString(def->argument).isEmpty() || QString(def->argument) == QLatin1String("text")) {
        QVariantMap &keys = m_session.profile().hotkeys;
        if (text.isEmpty()) keys.remove(action); else keys.insert(action, text);
        saveProfile();
    } else {
        QVariantMap keys = m_production.collection().hotkeys;
        const QString id = action + QLatin1Char('|') + argument;
        if (text.isEmpty()) keys.remove(id); else keys.insert(id, text);
        m_production.setHotkeys(keys);
    }
    refreshHotkeys();
    emit hotkeysChanged();
}

QString Backend::keyText(int key, int modifiers) const {
    if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_AltGr || key == 0) return {};
    const auto mods = Qt::KeyboardModifiers(modifiers) & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    return QKeySequence(QKeyCombination(mods, Qt::Key(key))).toString(QKeySequence::PortableText);
}

QString Backend::hotkeyClash(const QString &action, const QString &argument, const QString &key) const {
    const QKeySequence want = QKeySequence::fromString(key, QKeySequence::PortableText);
    if (want.isEmpty()) return {};
    const QString self = action + QLatin1Char('|') + argument;
    const QVariantMap keys = hotkeys();
    for (const QVariant &entry : actions()) {
        const QVariantMap a = entry.toMap();
        const QString id = a.value("id").toString() + QLatin1Char('|') + a.value("argument").toString();
        if (id != self && QKeySequence::fromString(keys.value(id).toString(), QKeySequence::PortableText) == want) return a.value("name").toString();
    }
    // The window's own shortcuts (Main.qml): a hotkey on one of them takes it
    // over, so it is said like any other clash.
    static const QList<std::pair<const char *, const char *>> builtIn{
        {"Ctrl+R", QT_TR_NOOP("Start or stop recording (built in)")}, {"Ctrl+Shift+R", QT_TR_NOOP("Save the replay (built in)")},
        {"Ctrl+Shift+D", QT_TR_NOOP("Add a display (built in)")}, {"Ctrl+Shift+W", QT_TR_NOOP("Add a window (built in)")},
        {"Ctrl+,", QT_TR_NOOP("Settings (built in)")}, {"F1", QT_TR_NOOP("Help (built in)")}, {"?", QT_TR_NOOP("Help (built in)")},
        {"Ctrl+Q", QT_TR_NOOP("Quit (built in)")}, {"Ctrl+C", QT_TR_NOOP("Copy sources (built in)")},
        {"Ctrl+V", QT_TR_NOOP("Paste sources (built in)")}, {"Ctrl+Shift+V", QT_TR_NOOP("Paste copies (built in)")},
        {"Ctrl+Alt+C", QT_TR_NOOP("Copy the placement (built in)")}, {"Ctrl+Alt+V", QT_TR_NOOP("Paste the placement (built in)")},
        {"Ctrl+0", QT_TR_NOOP("Fit the view (built in)")}, {"Ctrl+1", QT_TR_NOOP("Actual pixels (built in)")},
        {"Ctrl+=", QT_TR_NOOP("Zoom in (built in)")}, {"Ctrl++", QT_TR_NOOP("Zoom in (built in)")}, {"Ctrl+-", QT_TR_NOOP("Zoom out (built in)")},
        {"Ctrl+G", QT_TR_NOOP("Group (built in)")}, {"Ctrl+Shift+G", QT_TR_NOOP("Ungroup (built in)")},
        {"Ctrl+Z", QT_TR_NOOP("Undo (built in)")}, {"Ctrl+Shift+Z", QT_TR_NOOP("Redo (built in)")}, {"Ctrl+Y", QT_TR_NOOP("Redo (built in)")}};
    for (const auto &[sequence, name] : builtIn)
        if (QKeySequence::fromString(QString::fromLatin1(sequence), QKeySequence::PortableText) == want) return tr(name);
    return {};
}

void Backend::runHotkey(const QString &action, const QString &argument, bool local) {
    const bool focused = local || QGuiApplication::applicationState() == Qt::ApplicationActive;
    if ((hotkeyFocus() == QLatin1String("focused") && !focused) || (hotkeyFocus() == QLatin1String("unfocused") && focused)) return;
    if (const QString why = runAction(action, argument); !why.isEmpty()) setStatus(why);
}
