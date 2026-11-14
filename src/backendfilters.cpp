// Video filters on sources: the list in a source's settings, its undo steps,
// copy and paste between sources, and the files some filters read.

#include "backend.h"
#include "core/video_filter_settings.h"

#include <QUuid>

namespace {
QString freshFilterId() { return QUuid::createUuid().toString(QUuid::WithoutBraces).left(12); }
}

QVariantList Backend::videoFilterTypes() const { return VideoFilterSettings::catalog(); }

QVariantList Backend::videoFilters(const QString &sourceId) const {
    const SourceDef *src = m_production.collection().source(sourceId);
    return src ? VideoFilterSettings::normalized(src->settings.value(QStringLiteral("videoFilters")).toList()) : QVariantList();
}

QVariantMap Backend::videoFilterSource(const QString &sourceId) const {
    const SourceDef *src = m_production.collection().source(sourceId);
    if (!src || SourceDef::isAudioKind(src->kind)) return {};
    static const QStringList async{"media", "device", "display", "window", "region"};
    return {{QStringLiteral("name"), src->name}, {QStringLiteral("kind"), src->kind}, {QStringLiteral("async"), async.contains(src->kind)}};
}

void Backend::setVideoFilters(const QString &sourceId, const QVariantList &filters, bool gesture) {
    const SourceDef *src = m_production.collection().source(sourceId);
    if (!src || SourceDef::isAudioKind(src->kind)) return;
    const QVariantList list = VideoFilterSettings::normalized(filters);
    if (list == videoFilters(sourceId) && src->settings.contains(QStringLiteral("videoFilters")) == !list.isEmpty()) return;
    QVariantMap settings = src->settings;
    if (list.isEmpty()) settings.remove(QStringLiteral("videoFilters"));
    else settings.insert(QStringLiteral("videoFilters"), list);
    m_production.setSourceSettings(sourceId, settings, !m_settingMergeKey.isEmpty() ? m_settingMergeKey
                                                      : gesture ? QStringLiteral("videoFilters:") + sourceId : QString());
}

void Backend::copyVideoFilters(const QString &sourceId, const QString &filterId) {
    QVariantList list = videoFilters(sourceId);
    if (!filterId.isEmpty())
        list.removeIf([&](const QVariant &v) { return v.toMap().value(QStringLiteral("id")).toString() != filterId; });
    if (list.isEmpty()) return;
    m_videoFilterClipboard = list;
    emit videoFilterClipboardChanged();
}

void Backend::pasteVideoFilters(const QString &sourceId) {
    if (m_videoFilterClipboard.isEmpty()) return;
    QVariantList list = videoFilters(sourceId);
    for (const QVariant &v : m_videoFilterClipboard) {
        QVariantMap copy = v.toMap();
        copy.insert(QStringLiteral("id"), freshFilterId());
        list.append(copy);
    }
    setVideoFilters(sourceId, list);
}

void Backend::findKeyBackdrop(const QString &sourceId, const QString &filterId, double x, double y) {
    const QString id = QStringLiteral("backdrop-%1").arg(++m_backdropSerial);
    m_backdropAsks.insert(id, {sourceId, filterId, x, y, m_production.generation()});
    m_session.requestSnapshot(id, QStringLiteral("sourceInput"), sourceId);
}

void Backend::keyBackdropFound(const QString &snapshotId, const QImage &picture) {
    const auto it = m_backdropAsks.find(snapshotId);
    if (it == m_backdropAsks.end()) return;
    const BackdropAsk ask = it.value();
    m_backdropAsks.erase(it);
    if (ask.generation != m_production.generation()) return;  // another collection since
    QVariantList next = videoFilters(ask.sourceId);
    for (QVariant &v : next) {
        QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("id")).toString() != ask.filterId || m.value(QStringLiteral("type")).toString() != QLatin1String("chromaKey")) continue;
        QString why;
        const QString colour = VideoFilterSettings::findBackdrop(picture, m.value(QStringLiteral("keyType")).toString(),
                                                                 m.value(QStringLiteral("backdrop")).toString(), ask.x, ask.y, &why);
        if (colour.isEmpty()) { setStatus(tr("No backdrop colour: %1.").arg(why)); return; }
        if (colour == m.value(QStringLiteral("backdrop")).toString()) { setStatus(tr("Backdrop colour: %1").arg(colour)); return; }
        m.insert(QStringLiteral("backdrop"), colour);
        v = m;
        setVideoFilters(ask.sourceId, next);
        setStatus(tr("Backdrop colour: %1").arg(colour));
        return;
    }
}

QVariantMap Backend::videoFilterLoad(const QString &sourceId) const {
    const double ms = m_session.filterCostMs(sourceId);
    if (ms < 0.05) return {{"known", false}};  // not measured yet, or nothing that runs every frame
    const Profile &p = m_session.profile();
    const double budget = p.fpsNum > 0 ? 1000.0 * std::max(1, p.fpsDen) / p.fpsNum : 1000.0 / 30;
    const Session::Stats st = m_session.stats();
    const double share = ms / budget;
    // These filters alone half the frame, or a real part of it while frames are
    // being late: frames are being lost. (How long the frame loop is busy says
    // little alone: at 4K most of it is copying pictures in and out.)
    const bool warn = share > 0.5 || (framesLateLately() && share > 0.1);
    const bool heavy = share > 0.25;
    // Said simply, and only when it matters: a light filter gets no line at all.
    QString text;
    if (warn)
        text = tr("This computer may not keep up with these filters, so recordings and streams may stutter. "
                  "If they do, use 30 frames a second or a smaller canvas (Settings → Video), "
                  "or a different encoder (Settings → Recording).");
    else if (heavy)
        text = tr("These filters are heavy. If recordings or streams stutter, see dropped frames in Stats.");
    return {{"known", true}, {"ms", ms}, {"budgetMs", budget}, {"frameMs", st.renderMs},
            {"level", warn ? "warn" : heavy ? "note" : "fine"}, {"text", text}};
}

void Backend::chooseVideoFilterFile(const QString &sourceId, const QString &filterId, const QString &key) {
    const QVariantList list = videoFilters(sourceId);
    QString type;
    for (const QVariant &v : list)
        if (v.toMap().value(QStringLiteral("id")).toString() == filterId) type = v.toMap().value(QStringLiteral("type")).toString();
    if (type.isEmpty()) return;
    QStringList patterns;
    for (const QVariant &d : VideoFilterSettings::catalog())
        if (d.toMap().value(QStringLiteral("type")).toString() == type)
            for (const QVariant &p : d.toMap().value(QStringLiteral("parameters")).toList())
                if (p.toMap().value(QStringLiteral("key")).toString() == key) patterns = p.toMap().value(QStringLiteral("fileTypes")).toStringList();
    if (patterns.isEmpty()) return;
    const quint64 generation = m_production.generation();
    m_otherChosen = [this, sourceId, filterId, key, generation](const QString &path) {
        if (generation != m_production.generation()) return;  // another collection since
        QVariantList next = videoFilters(sourceId);
        for (QVariant &v : next) {
            QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("id")).toString() != filterId) continue;
            m.insert(key, path);
            v = m;
        }
        setVideoFilters(sourceId, next);
    };
    m_otherPicker.openFile(type == QLatin1String("lut") ? tr("Choose a LUT") : tr("Choose a picture"),
                           type == QLatin1String("lut") ? tr("LUT files") : tr("Pictures"), patterns);
}
