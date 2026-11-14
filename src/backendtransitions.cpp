#include "backend.h"

#include "core/transitions.h"

// Transitions between studios and studio mode.

QVariantList Backend::transitions() const { return Transitions::all(m_production.collection()); }

QVariantList Backend::quickTransitions() const {
    QVariantList out;
    const Collection &c = m_production.collection();
    const QVariantList all = Transitions::all(c);
    for (const QuickTransition &q : c.quickTransitions) {
        QString name;
        for (const QVariant &t : all)
            if (t.toMap().value("id").toString() == q.transition) name = t.toMap().value("name").toString();
        out.append(QVariantMap{{"transition", q.transition}, {"name", name}, {"durationMs", q.durationMs}});
    }
    return out;
}

QVariantList Backend::transitionKinds() const { return Transitions::kinds(); }
QVariantList Backend::transitionSides() const { return Transitions::sides(); }
QVariantList Backend::transitionMasks() const { return Transitions::masks(); }

QString Backend::addTransition(const QString &kind) {
    QString name;
    for (const QVariant &k : Transitions::kinds())
        if (k.toMap().value("id").toString() == kind) name = k.toMap().value("name").toString();
    if (name.isEmpty()) return {};
    // "Wipe", then "Wipe 2", …
    QStringList taken;
    for (const QVariant &t : Transitions::all(m_production.collection())) taken << t.toMap().value("name").toString();
    QString unique = name;
    for (int n = 2; taken.contains(unique); ++n) unique = QStringLiteral("%1 %2").arg(name).arg(n);
    const QString id = m_production.addTransition(unique, {{"kind", kind}});
    if (!id.isEmpty()) m_production.setCurrentTransition(id, transitionDuration() > 0 ? transitionDuration() : 300);
    return id;
}

void Backend::removeTransition(const QString &id) { m_production.removeTransition(id); }
void Backend::renameTransition(const QString &id, const QString &name) {
    if (!name.trimmed().isEmpty()) m_production.renameTransition(id, name.trimmed());
}

QVariantMap Backend::transitionSettings(const QString &id) const { return Transitions::settings(m_production.collection(), id); }

void Backend::setTransitionSetting(const QString &id, const QString &key, const QVariant &value, const QString &mergeKey) {
    if (!m_production.collection().transition(id)) return;  // the built-in ones do not change
    QVariantMap settings = transitionSettings(id);
    settings.insert(key, value);
    m_production.setTransitionSettings(id, settings, mergeKey);
}

void Backend::chooseStingerFile(const QString &id, const QString &key) {
    const bool matte = key == QLatin1String("matteFile");
    const quint64 generation = m_production.generation();
    m_otherChosen = [this, id, matte, generation](const QString &path) {
        if (generation == m_production.generation()) setTransitionSetting(id, matte ? QStringLiteral("matteFile") : QStringLiteral("file"), path);
    };
    m_otherPicker.openFile(matte ? tr("The stinger's track matte") : tr("The stinger's video"), tr("Videos"),
                           {QStringLiteral("*.mov"), QStringLiteral("*.webm"), QStringLiteral("*.mkv"), QStringLiteral("*.mp4"),
                            QStringLiteral("*.avi"), QStringLiteral("*.gif"), QStringLiteral("*")});
}

void Backend::setSceneTransition(const QString &sceneId, const QString &transition, int durationMs) {
    m_production.setSceneTransition(sceneId, transition, durationMs);
}

void Backend::addQuickTransition(const QString &transition, int durationMs) {
    auto quick = m_production.collection().quickTransitions;
    for (const QuickTransition &q : quick)
        if (q.transition == transition && q.durationMs == durationMs) return;
    quick.append({transition, durationMs});
    m_production.setQuickTransitions(quick);
}

void Backend::removeQuickTransition(int index) {
    auto quick = m_production.collection().quickTransitions;
    if (index < 0 || index >= quick.size()) return;
    quick.removeAt(index);
    m_production.setQuickTransitions(quick);
}

void Backend::setStudioMode(bool on) {
    if (on == studioMode()) return;
    m_swapTo.clear();
    m_session.setStudioMode(on);
    setPreference(QStringLiteral("studioMode"), on);
}

bool Backend::canTake() const {
    return studioMode() && !currentScene().isEmpty() && (currentScene() != programScene() || takeCopy() || tBarActive());
}

// The studio to preview once this take has settled: the one it leaves.
void Backend::swapAfter(const QString &onAir) {
    m_swapTo = onAir;
    m_swapGeneration = m_production.generation();
}

void Backend::take() {
    if (!canTake()) return;
    // A T-bar let go part way: Take finishes it when the preview is where it
    // was going; another studio previewed is taken from the mix showing.
    if (tBarActive() && currentScene() == m_session.transitionTarget()) {
        setTBar(1.0);
        return;
    }
    const QString preview = currentScene(), onAir = programScene();
    swapAfter(swapAfterTake() && onAir != preview ? onAir : QString());
    m_session.takeProgram(preview, takeCopy());
    if (!m_session.transitioning()) settleTake();
}

void Backend::quickTake(int index) {
    const auto &quick = m_production.collection().quickTransitions;
    // Nothing to take: not left armed for the next take, which chose its own.
    if (!canTake() || index < 0 || index >= quick.size()) return;
    // Nor when this take only finishes a parked T-bar by hand, which uses none.
    if (!(tBarActive() && currentScene() == m_session.transitionTarget()))
        m_session.setNextTransition(quick[index].transition, quick[index].durationMs);
    take();
}

// Where the bar is and whether it is in use are the session's to say (its
// transition by hand), so a transition it drops is never still "held" here.
void Backend::setTBar(double position) {
    if (!studioMode()) return;
    const bool held = tBarActive();
    const QString preview = held ? m_session.transitionTarget() : currentScene();
    if (!held) {
        if (preview == programScene()) return;
        swapAfter(swapAfterTake() ? programScene() : QString());
    }
    const double at = std::clamp(position, 0.0, 1.0);
    // At the end the session has settled before it says so: the window can let
    // go of the bar on that signal, and the release finds nothing to do.
    m_session.setManualTransition(preview, at, takeCopy());
    // Swapped only if it did get there (a session that is not running moves nothing).
    if (at >= 1.0) transitionEnded();
    emit transitionStateChanged();
}

void Backend::releaseTBar() {
    if (!tBarActive()) return;
    // Let go near an end: it goes to that end; elsewhere it stays put.
    const double at = m_session.transitionProgress();
    if (at >= 0.98) setTBar(1.0);
    else if (at <= 0.02) {
        m_swapTo.clear();
        m_session.setManualTransition(currentScene(), -1);
        emit transitionStateChanged();
    }
}

void Backend::settleTake() {
    const QString swap = m_swapTo;
    m_swapTo.clear();
    if (!swap.isEmpty() && m_swapGeneration == m_production.generation() && m_production.collection().scene(swap))
        m_production.setCurrentScene(swap);
}

// A transition ended. One that got where it was going swaps the preview; one
// given up or dropped (a studio of it removed, another collection opened)
// left the program where it was, and nothing waits to swap.
void Backend::transitionEnded() {
    if (m_session.transitioning()) return;
    if (!m_swapTo.isEmpty() && programScene() != m_swapTo) settleTake();
    else m_swapTo.clear();
}
