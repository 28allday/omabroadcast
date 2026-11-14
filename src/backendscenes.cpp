// Scenes, sources and transforms: the editing half of the backend.

#include "backend.h"
#include "core/audio_settings.h"
#include "core/repair.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <functional>
#include <tuple>
#include <cmath>

namespace {

const char *boundsName(Transform::Bounds b) {
    switch (b) {
    case Transform::Bounds::Stretch: return "stretch";
    case Transform::Bounds::Fit: return "fit";
    case Transform::Bounds::Fill: return "fill";
    case Transform::Bounds::Cover: return "cover";
    case Transform::Bounds::Width: return "width";
    case Transform::Bounds::Height: return "height";
    case Transform::Bounds::MaxOnly: return "max-only";
    case Transform::Bounds::None: break;
    }
    return "none";
}

// Where each loaded presentation's pictures are kept, a folder for each load.
QString presentationsRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/presentations");
}

// Removes `path` only when it really is a folder directly inside the
// presentations folder: by where the two are on disk, not by how the path is
// written, so ".." and links cannot lead the removal anywhere else.
bool removePresentationFolder(const QString &path) {
    const QString root = QDir(presentationsRoot()).canonicalPath();
    const QFileInfo info(path);
    if (root.isEmpty() || path.isEmpty() || info.isSymLink() || !info.isDir()) return false;
    const QString real = info.canonicalFilePath();
    if (real.isEmpty() || QFileInfo(real).absolutePath() != root) return false;
    return QDir(real).removeRecursively();
}

// The folders inside the presentations folder (`roots`: as written and as it
// is on disk) that anything in `value` names, by their own names. From the
// text alone: a path on a drive that has gone must not be waited for.
void presentationFoldersNamed(const QVariant &value, const QStringList &roots, QSet<QString> &out) {
    switch (value.typeId()) {
    case QMetaType::QString: {
        const QString path = QDir::cleanPath(value.toString());
        for (const QString &root : roots) {
            if (root.isEmpty() || !path.startsWith(root + QLatin1Char('/'))) continue;
            const QString name = path.mid(root.size() + 1).section(QLatin1Char('/'), 0, 0);
            if (!name.isEmpty()) out.insert(name);
        }
        break;
    }
    case QMetaType::QStringList:
        for (const QString &one : value.toStringList()) presentationFoldersNamed(one, roots, out);
        break;
    case QMetaType::QVariantList:
        for (const QVariant &one : value.toList()) presentationFoldersNamed(one, roots, out);
        break;
    case QMetaType::QVariantMap: {
        const QVariantMap map = value.toMap();
        for (const QVariant &one : map) presentationFoldersNamed(one, roots, out);
        break;
    }
    default:
        break;
    }
}

}  // namespace

void Backend::loadCollection() {
    const QString path = collectionFile(m_session.profile().collection);
    if (QFile::exists(path) && m_production.load(path)) {
        // Studios were called scenes once: default names follow the new word,
        // names someone typed stay as they are.
        Collection c = m_production.collection();
        bool renamed = false;
        static const QRegularExpression defaultName(QStringLiteral("^Scene (\\d+)$"));
        for (Scene &s : c.scenes)
            if (const QRegularExpressionMatch m = defaultName.match(s.name); m.hasMatch()) {
                s.name = tr("Studio %1").arg(m.captured(1));
                renamed = true;
            }
        if (renamed) {
            m_production.reset(c);
            m_production.save();
        }
        emit collectionsChanged();
        return;
    }
    // One that is there but does not read is set aside, never written over.
    if (QFile::exists(path)) {
        const QString config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        const QString aside = repair::asideFolder(config, QDateTime::currentDateTime());
        QString why;
        if (repair::setAside(config, aside, path, false, &why))
            setStatus(tr("%1 could not be read; it was set aside in %2.").arg(QFileInfo(path).fileName(), aside));
        else
            qWarning("collection %s does not read and could not be set aside: %s", qPrintable(path), qPrintable(why));
    }
    if (m_session.profile().collection != QLatin1String("Default") && QFile::exists(collectionFile(QStringLiteral("Default")))) {
        // The one asked for is gone: back to the Default.
        m_session.profile().collection = QStringLiteral("Default");
        m_session.setTokenScope({});
        loadCollection();
        return;
    }
    // First run with scenes: what the profile used to keep becomes Scene 1.
    Collection c;
    Profile &profile = m_session.profile();
    Scene scene{c.makeId(QLatin1Char('c')), tr("Studio 1"), {}};
    for (const SourceEntry &old : std::as_const(profile.legacySources)) {
        const QString id = c.makeId(QLatin1Char('s'));
        const bool window = old.types & ScreenCastPortal::Window;
        c.sources.append({id, window ? QStringLiteral("window") : QStringLiteral("display"), old.name,
                          {{QStringLiteral("cursor"), old.cursor}}});
        scene.items.append({c.makeId(QLatin1Char('i')), id, canvasFit(), true, false});
        if (!old.restoreToken.isEmpty()) profile.restoreTokens.insert(id, old.restoreToken);
    }
    c.scenes.append(scene);
    c.currentScene = scene.id;
    profile.legacySources.clear();
    m_production.reset(c);
    // Still there (it could not be set aside): never written over. This one
    // is not saved until that file is moved.
    if (QFile::exists(path)) {
        m_production.setPathOnceGone(path);
        setStatus(tr("%1 could not be read or moved aside; move it yourself, then changes here are saved again.")
                      .arg(QFileInfo(path).fileName()));
    } else {
        m_production.setPath(path);
        m_production.save();
    }
    saveProfile();
    emit collectionsChanged();
}

void Backend::fitLayoutToCanvas() {
    const Profile &p = m_session.profile();
    if (m_session.running()) m_production.followCanvas(canvasWidth(), canvasHeight());
    // Not started: what the profile will start at, unless that is automatic, when
    // the canvas starts at the size the layout is at.
    else if (p.canvasWidth > 0 && p.canvasHeight > 0) m_production.followCanvas(p.canvasWidth & ~1, p.canvasHeight & ~1);
}

Transform Backend::canvasFit() const {
    Transform t;
    t.bounds = Transform::Bounds::Fit;
    t.boundsWidth = canvasWidth();
    t.boundsHeight = canvasHeight();
    return t;
}

QString Backend::nextName(const QString &base) const {
    // "Display 2" when "Display 1" exists; names are for people, ids for the code.
    for (int n = 1;; ++n) {
        const QString name = QStringLiteral("%1 %2").arg(base).arg(n);
        bool taken = false;
        for (const SourceDef &s : m_production.collection().sources) taken |= s.name == name;
        if (!taken) return name;
    }
}

// ----------------------------------------------------------------- reads

QVariantList Backend::scenes() const {
    QVariantList out;
    const Collection &c = m_production.collection();
    for (const Scene &s : c.scenes) {
        if (s.group) continue;  // a group's box is not a studio
        // Whether the live studio could show this one without a loop.
        const bool showable = s.id != c.currentScene && !c.shows(s.id, c.currentScene);
        out.append(QVariantMap{{QStringLiteral("id"), s.id}, {QStringLiteral("name"), s.name},
                               {QStringLiteral("showable"), showable}, {QStringLiteral("transition"), s.transition},
                               {QStringLiteral("transitionMs"), s.transitionMs}});
    }
    return out;
}

namespace {

// A point in a group's own coordinates, in its parent's; and back.
QPointF upThrough(const Backend::GroupStep &st, QPointF p) { return st.mapping.map(p); }
QPointF downThrough(const Backend::GroupStep &st, QPointF p) { return st.mapping.inverted().map(p); }
Outline mapOutline(const Backend::GroupStep &st, const Outline &o, bool up) {
    return mappedOutline(up ? st.mapping : st.mapping.inverted(), o);
}

}  // namespace

QSize Backend::sourceSize(const QString &sourceId) const {
    const Collection &c = m_production.collection();
    if (const SourceDef *src = c.source(sourceId)) {
        if (SourceDef::isAudioKind(src->kind)) return {0, 0};
        if (src->kind == QLatin1String("group"))
            if (const Scene *g = c.scene(c.shownStudio(*src))) return {g->width, g->height};
        if (src->kind == QLatin1String("studio")) return {canvasWidth(), canvasHeight()};
    }
    int w = 0, h = 0;
    if (!m_session.feedSize(sourceId, &w, &h)) {
        if (const auto *source = c.source(sourceId); source && source->kind == "region")
            return CaptureRegion::fromSettings(source->settings).bounds(canvasWidth(), canvasHeight()).size();
        // A page is the size asked for, before its first picture.
        if (const auto *source = c.source(sourceId); source && source->kind == "browser")
            return {std::max(1, source->settings.value("width", 800).toInt()), std::max(1, source->settings.value("height", 600).toInt())};
        return {canvasWidth(), canvasHeight()};
    }
    return {w, h};
}

QList<Backend::GroupStep> Backend::groupSteps(const QString &itemId) const {
    QList<GroupStep> steps;
    const Collection &c = m_production.collection();
    const Scene *owner = nullptr;
    if (!c.findItem(itemId, &owner)) return steps;
    for (int depth = 0; owner && owner->group && depth < 16; ++depth) {
        // The one item that shows this group, wherever it is.
        const SceneItem *shower = nullptr;
        const Scene *parent = nullptr;
        for (const Scene &s : c.scenes)
            for (const SceneItem &i : s.items)
                if (const SourceDef *src = c.source(i.source);
                    src && src->kind == QLatin1String("group") && c.shownStudio(*src) == owner->id) {
                    shower = &i;
                    parent = &s;
                }
        if (!shower) break;
        steps.append({itemMapping(shower->transform, owner->width, owner->height), shower->locked});
        owner = parent;
    }
    return steps;
}

Outline Backend::canvasOutline(const QString &itemId) const {
    const SceneItem *item = m_production.collection().findItem(itemId);
    if (!item) return {};
    const QSize size = sourceSize(item->source);
    Outline o = itemOutline(item->transform, size.width(), size.height());
    for (const GroupStep &st : groupSteps(itemId)) o = mapOutline(st, o, true);
    return o;
}

bool Backend::placementLocked(const QString &itemId) const {
    const SceneItem *item = m_production.collection().findItem(itemId);
    if (!item) return true;
    if (item->locked) return true;
    for (const GroupStep &st : groupSteps(itemId))
        if (st.locked) return true;
    return false;
}

QVariantList Backend::items() const {
    QVariantList out;
    const Scene *scene = m_production.currentScene();
    if (!scene) return out;
    std::map<QString, Session::FeedInfo> feeds;
    for (const Session::FeedInfo &f : m_session.feeds()) feeds[f.sourceId] = f;
    // Top of the stack first; a group's items follow it, one level in.
    const Collection &col = m_production.collection();
    std::vector<std::tuple<const SceneItem *, int, QString>> rows;
    std::function<void(const Scene &, int, const QString &)> walk = [&](const Scene &s, int depth, const QString &parent) {
        for (auto it = s.items.crbegin(); it != s.items.crend(); ++it) {
            rows.emplace_back(&*it, depth, parent);
            const SourceDef *src = col.source(it->source);
            if (src && src->kind == QLatin1String("group") && depth < 16)
                if (const Scene *inner = col.scene(col.shownStudio(*src))) walk(*inner, depth + 1, it->id);
        }
    };
    walk(*scene, 0, QString());
    for (const auto &[itemPtr, depth, parent] : rows) {
        const SceneItem *it = itemPtr;
        const SourceDef *src = m_production.collection().source(it->source);
        if (!src) continue;
        const Session::FeedInfo feed = feeds.count(src->id) ? feeds[src->id] : Session::FeedInfo{};
        const bool capture = src->kind == QLatin1String("display") || src->kind == QLatin1String("window") || src->kind == QLatin1String("region");
        QString detail = feed.state;
        if (src->kind == QLatin1String("device") && feed.state == QLatin1String("live") && !feed.mode.isEmpty())
            detail = feed.mode;
        else if (src->kind == QLatin1String("media")) {
            if (feed.state == QLatin1String("playing") && feed.width > 0)
                detail = QStringLiteral("%1 × %2 · %3%4").arg(feed.width).arg(feed.height).arg(feed.codec)
                             .arg(feed.hardwareDecode ? tr(" · GPU decode") : QString());
            else if (feed.state == QLatin1String("listening"))
                detail = tr("waiting for the phone on port %1").arg(src->settings.value(QStringLiteral("port")).toInt());
            if (feed.state == QLatin1String("playing") && !feed.refusedSound.isEmpty())
                detail += tr(" · no sound: it sends %1, which is not taken").arg(feed.refusedSound);
        }
        else if (capture && feed.width > 0 && feed.state == QLatin1String("live"))
            detail = QStringLiteral("%1 × %2 · %3").arg(feed.width).arg(feed.height)
                         .arg(feed.dmabuf ? tr("GPU buffers") : tr("memory buffers"));
        else if (!capture && feed.width > 0 && feed.state == QLatin1String("live"))
            detail = QStringLiteral("%1 × %2").arg(feed.width).arg(feed.height);
        out.append(QVariantMap{{QStringLiteral("id"), it->id},
                               {QStringLiteral("sourceId"), src->id},
                               {QStringLiteral("name"), src->name},
                               {QStringLiteral("kind"), src->kind},
                               {QStringLiteral("visible"), it->visible},
                               {QStringLiteral("locked"), it->locked},
                               {QStringLiteral("placementLocked"), placementLocked(it->id)},
                               {QStringLiteral("live"), feed.live && (feed.state == QLatin1String("live") ||
                                                                       feed.state == QLatin1String("playing"))},
                               {QStringLiteral("state"), feed.state},
                               {QStringLiteral("detail"), detail},
                               {QStringLiteral("capture"), capture},
                               {QStringLiteral("network"), src->kind == QLatin1String("media") &&
                                                               src->settings.contains(QStringLiteral("port"))},
                               {QStringLiteral("depth"), depth},
                               {QStringLiteral("parent"), parent},
                               {QStringLiteral("group"), src->kind == QLatin1String("group")}});
    }
    return out;
}

bool Backend::itemState(const QString &id, Transform *transform, int *w, int *h) const {
    const SceneItem *item = m_production.collection().findItem(id);
    if (!item) return false;
    *transform = item->transform;
    const QSize size = sourceSize(item->source);
    *w = size.width();
    *h = size.height();
    return true;
}

QVariantList Backend::outlines() const {
    QVariantList out;
    const Scene *scene = m_production.currentScene();
    if (!scene) return out;
    // Top-level items (what a click picks) and, through visible groups, the
    // items inside (selectable from the list; handles where they show).
    const Collection &c = m_production.collection();
    std::function<void(const Scene &, int, bool)> add = [&](const Scene &s, int depth, bool locked) {
        for (const SceneItem &item : s.items) {
            if (!item.visible) continue;
            const SourceDef *src = c.source(item.source);
            if (src && SourceDef::isAudioKind(src->kind)) continue;
            const Outline o = canvasOutline(item.id);
            out.append(QVariantMap{{QStringLiteral("id"), item.id},
                                   {QStringLiteral("x"), o.x},
                                   {QStringLiteral("y"), o.y},
                                   {QStringLiteral("width"), o.width},
                                   {QStringLiteral("height"), o.height},
                                   {QStringLiteral("rotation"), o.rotation},
                                   {QStringLiteral("locked"), item.locked || locked},
                                   {QStringLiteral("depth"), depth}});
            if (src && src->kind == QLatin1String("group"))
                if (const Scene *inner = c.scene(c.shownStudio(*src)); inner && depth < 16)
                    add(*inner, depth + 1, item.locked || locked);
        }
    };
    add(*scene, 0, false);
    return out;
}

// A capture device's modes for the inspector: [{key, label}], best first.
static QVariantList deviceModesFor(const std::vector<VideoDevice> &devices, const SourceDef *src) {
    QVariantList out;
    if (!src || src->kind != QLatin1String("device")) return out;
    const QString node = src->settings.value(QStringLiteral("node")).toString();
    for (const VideoDevice &d : devices) {
        if (QString::fromStdString(d.name) != node) continue;
        std::vector<VideoMode> modes = d.modes;
        std::stable_sort(modes.begin(), modes.end(), [](const VideoMode &a, const VideoMode &b) {
            const long long pa = 1LL * a.width * a.height, pb = 1LL * b.width * b.height;
            return pa != pb ? pa > pb : a.fps() > b.fps();
        });
        for (const VideoMode &m : modes)
            out.append(QVariantMap{{QStringLiteral("key"), QString::fromStdString(m.key())},
                                   {QStringLiteral("label"), QString::fromStdString(m.label())}});
    }
    return out;
}

QVariantList Backend::deviceModes(const SourceDef *src) const { return deviceModesFor(m_session.videoDevices(), src); }

// The mode a capture device runs in now, as its setting and the canvas's
// frame rate pick it; false when it is not connected.
static bool runningMode(const std::vector<VideoDevice> &devices, const SourceDef &src, double canvasFps, VideoMode *out) {
    const QString node = src.settings.value(QStringLiteral("node")).toString();
    for (const VideoDevice &d : devices) {
        if (QString::fromStdString(d.name) != node || d.modes.empty()) continue;
        *out = pickVideoMode(d.modes, src.settings.value(QStringLiteral("mode")).toString().toStdString(), canvasFps);
        return true;
    }
    return false;
}

QString Backend::frameRateNote() const {
    const double canvas = double(fpsNum()) / std::max(1, fpsDen());
    const std::vector<VideoDevice> devices = m_session.videoDevices();
    QStringList differ;
    for (const SourceDef &src : m_production.collection().sources) {
        VideoMode mode;
        if (src.kind != QLatin1String("device") || !runningMode(devices, src, canvas, &mode) || std::abs(mode.fps() - canvas) < 0.01) continue;
        differ << tr("%1 sends %2").arg(src.name, QString::number(mode.fps(), 'g', 5));
    }
    if (differ.isEmpty()) return {};
    return tr("%1 frames a second, not %2: frames are repeated or dropped to fit. Choose the same rate here, or a mode at %2 on the source.")
        .arg(differ.join(QStringLiteral("; ")), QString::number(canvas, 'g', 5));
}

// A capture device whose picture changes at a rate the canvas's does not fit
// evenly (a 50 Hz signal on a 60 fps canvas): that rate, and the frame rate
// that would show every picture for as long; empty when it fits.
QVariantMap Backend::evenRateFor(const QString &sourceId) const {
    for (const Session::FeedInfo &f : m_session.feeds()) {
        if (f.sourceId != sourceId) continue;
        int num = 0, den = 1;
        if (!evenFrameRate(f.pictureRate, double(fpsNum()) / std::max(1, fpsDen()), &num, &den)) return {};
        return {{QStringLiteral("pictures"), qRound(f.pictureRate)}, {QStringLiteral("num"), num}, {QStringLiteral("den"), den},
                {QStringLiteral("label"), den == 1 ? QString::number(num) : QString::number(double(num) / den, 'f', 2)}};
    }
    return {};
}

// The rate evenRateFor gives, for the canvas and the device: its mode of the
// same size at that rate when it was set to one (uncompressed first), which
// takes the canvas to it too; else the canvas alone, Best available following.
void Backend::useEvenRate(const QString &sourceId) {
    const QVariantMap even = evenRateFor(sourceId);
    const SourceDef *src = m_production.collection().source(sourceId);
    if (even.isEmpty() || !src) return;
    const int num = even.value(QStringLiteral("num")).toInt(), den = even.value(QStringLiteral("den")).toInt();
    VideoMode now;
    if (!src->settings.value(QStringLiteral("mode")).toString().isEmpty() &&
        runningMode(m_session.videoDevices(), *src, double(fpsNum()) / std::max(1, fpsDen()), &now)) {
        const QString node = src->settings.value(QStringLiteral("node")).toString();
        const VideoMode *pick = nullptr;
        for (const VideoDevice &d : m_session.videoDevices())
            if (QString::fromStdString(d.name) == node)
                for (const VideoMode &m : d.modes)
                    if (m.width == now.width && m.height == now.height && 1LL * m.fpsNum * den == 1LL * num * m.fpsDen &&
                        (!pick || (pick->format == VideoMode::Format::MJPEG && m.format != VideoMode::Format::MJPEG)))
                        pick = &m;
        if (pick) { setSourceSetting(sourceId, QStringLiteral("mode"), QString::fromStdString(pick->key())); return; }
        setSourceSetting(sourceId, QStringLiteral("mode"), QString());
    }
    applyFrameRate(num, den, src->name);
}

QVariantMap Backend::selected() const {
    const QString id = selectedItem();
    Transform t;
    int w = 0, h = 0;
    if (id.isEmpty() || !itemState(id, &t, &w, &h)) return {};
    const SceneItem *item = m_production.collection().findItem(id);
    const SourceDef *src = m_production.collection().source(item->source);
    const Outline o = canvasOutline(id);  // where it shows, through any groups
    // The anchor and pivot as they show on the canvas.
    QPointF position(t.x, t.y), pivot = itemPivot(t, w, h);
    for (const GroupStep &st : groupSteps(id)) {
        position = upThrough(st, position);
        pivot = upThrough(st, pivot);
    }
    const auto capture = src ? m_session.captureSize(src->id) : QSize();
    return {{QStringLiteral("id"), id},
            {QStringLiteral("sourceId"), src ? src->id : QString()},
            {QStringLiteral("name"), src ? src->name : QString()},
            {QStringLiteral("kind"), src ? src->kind : QString()},
            {QStringLiteral("settings"), src ? src->settings : QVariantMap()},
            {QStringLiteral("positionX"), position.x()},
            {QStringLiteral("positionY"), position.y()},
            {QStringLiteral("pivotX"), pivot.x()},
            {QStringLiteral("pivotY"), pivot.y()},
            {QStringLiteral("positionAlignment"), int(t.positionAlignment)},
            {QStringLiteral("rotationAlignment"), int(t.rotationAlignment)},
            {QStringLiteral("boundsAlignment"), int(t.boundsAlignment)},
            {QStringLiteral("clipToBounds"), t.clipToBounds},
            {QStringLiteral("x"), o.x},
            {QStringLiteral("y"), o.y},
            {QStringLiteral("width"), o.width},
            {QStringLiteral("height"), o.height},
            {QStringLiteral("rotation"), o.rotation},
            {QStringLiteral("cropLeft"), t.cropLeft},
            {QStringLiteral("cropTop"), t.cropTop},
            {QStringLiteral("cropRight"), t.cropRight},
            {QStringLiteral("cropBottom"), t.cropBottom},
            {QStringLiteral("flipH"), t.flipH},
            {QStringLiteral("flipV"), t.flipV},
            {QStringLiteral("bounds"), QLatin1String(boundsName(t.bounds))},
            {QStringLiteral("opacity"), t.opacity},
            {QStringLiteral("blendSpace"), t.blendSpace == engine::BlendSpace::Srgb ? QStringLiteral("srgb") : QStringLiteral("linear")},
            {QStringLiteral("blend"), QStringList{QStringLiteral("normal"), QStringLiteral("multiply"), QStringLiteral("screen"),
                                                  QStringLiteral("overlay"), QStringLiteral("darken"), QStringLiteral("lighten"),
                                                  QStringLiteral("add"), QStringLiteral("subtract"),
                                                  QStringLiteral("difference")}.value(int(t.blend))},
            {QStringLiteral("scale"), t.scale == engine::ScaleFilter::Bilinear ? QStringLiteral("bilinear")
                                      : t.scale == engine::ScaleFilter::Bicubic ? QStringLiteral("bicubic")
                                      : t.scale == engine::ScaleFilter::Lanczos ? QStringLiteral("lanczos")
                                      : t.scale == engine::ScaleFilter::Area    ? QStringLiteral("area")
                                      : t.scale == engine::ScaleFilter::Point   ? QStringLiteral("point")
                                                                                : QStringLiteral("auto")},
            {QStringLiteral("captureWidth"), std::max(0, capture.width())},
            {QStringLiteral("captureHeight"), std::max(0, capture.height())},
            {QStringLiteral("sourceWidth"), w},
            {QStringLiteral("sourceHeight"), h},
            {QStringLiteral("locked"), item->locked},
            {QStringLiteral("placementLocked"), placementLocked(id)},
            {QStringLiteral("visible"), item->visible},
            {QStringLiteral("showTransition"), item->showTransition},
            {QStringLiteral("showMs"), item->showMs},
            {QStringLiteral("hideTransition"), item->hideTransition},
            {QStringLiteral("hideMs"), item->hideMs},
            {QStringLiteral("modes"), deviceModes(src)},
            {QStringLiteral("bestMode"), [&] {
                // What Best available runs in, for the box's label.
                if (!src || src->kind != QLatin1String("device")) return QString();
                SourceDef best = *src;
                best.settings.remove(QStringLiteral("mode"));
                VideoMode mode;
                return runningMode(m_session.videoDevices(), best, double(fpsNum()) / std::max(1, fpsDen()), &mode)
                           ? QString::fromStdString(mode.label()) : QString();
            }()},
            {QStringLiteral("controls"), src && src->kind == QLatin1String("device") ? deviceControlList(src->id) : QVariantList()},
            {QStringLiteral("evenRate"), src && src->kind == QLatin1String("device") ? evenRateFor(src->id) : QVariantMap()},
            {QStringLiteral("firewall"), src && src->kind == QLatin1String("media") && src->settings.contains(QStringLiteral("port"))
                                             ? firewallHint(src->settings.value(QStringLiteral("protocol"), QStringLiteral("srt")).toString(),
                                                            src->settings.value(QStringLiteral("port")).toInt())
                                             : QString()},
            {QStringLiteral("sendTo"), src && src->kind == QLatin1String("media") && src->settings.contains(QStringLiteral("port"))
                                           ? sendAddresses(src->settings.value(QStringLiteral("protocol")).toString(),
                                                           src->settings.value(QStringLiteral("port")).toInt(),
                                                           src->settings.value(QLatin1String(kPassphraseKey)).toString())
                                           : QVariantList()}};
}

// ---------------------------------------------------------------- scenes

void Backend::addScene() {
    int n = m_production.collection().scenes.size() + 1;
    auto taken = [&](const QString &name) {
        for (const Scene &s : m_production.collection().scenes)
            if (s.name == name) return true;
        return false;
    };
    while (taken(tr("Studio %1").arg(n))) ++n;
    m_production.addScene(tr("Studio %1").arg(n));
}

void Backend::removeScene(const QString &id) { m_production.removeScene(id); }
void Backend::renameScene(const QString &id, const QString &name) {
    if (!name.trimmed().isEmpty()) m_production.renameScene(id, name.trimmed());
}
void Backend::duplicateScene(const QString &id) { m_production.duplicateScene(id); }
void Backend::moveScene(const QString &id, int toIndex) { m_production.moveScene(id, toIndex); }

// --------------------------------------------------------- sources, items

void Backend::addDisplay() {
    m_production.addSource(QStringLiteral("display"), nextName(tr("Display")), {{QStringLiteral("cursor"), true}},
                           canvasFit());
}

void Backend::addWindow() {
    m_production.addSource(QStringLiteral("window"), nextName(tr("Window")), {{QStringLiteral("cursor"), true}},
                           canvasFit());
}

void Backend::requestCaptureRegionPreview(const QString &sourceId) {
    const auto *source = m_production.collection().source(sourceId);
    if (source && source->kind == "region") m_session.requestCapturePreview(sourceId);
}

void Backend::drawCaptureRegion(const QString &sourceId) {
    const auto *source = m_production.collection().source(sourceId);
    if (!source || source->kind != "region") return;
    const QSize display = m_session.captureSize(sourceId);
    if (display.isEmpty()) { emit failed(tr("The display has no picture yet. Try again when capture is live.")); return; }
    if (!RegionDrawer::available()) { requestCaptureRegionPreview(sourceId); return; }
    m_regionGeneration = m_production.generation();
    if (m_regionDrawer.start(sourceId, display, m_session.captureLayoutArea(sourceId)))
        setStatus(tr("Drag a rectangle on the display. Esc cancels."));
}

void Backend::addRegion() {
    m_production.addSource("region", nextName(tr("Region")), {{"cursor", true}, {"regionX", 0}, {"regionY", 0},
        {"regionWidth", 0}, {"regionHeight", 0}}, canvasFit());
}

void Backend::setCaptureRegion(const QString &sourceId, double x, double y, double width, double height) {
    const auto *source = m_production.collection().source(sourceId);
    if (!source || source->kind != "region") return;
    auto settings = source->settings;
    settings["regionX"] = CaptureRegion::coordinate(x); settings["regionY"] = CaptureRegion::coordinate(y);
    settings["regionWidth"] = CaptureRegion::coordinate(width); settings["regionHeight"] = CaptureRegion::coordinate(height);
    m_production.setSourceSettings(sourceId, settings);
}

void Backend::addColour() {
    // A canvas-sized colour, full frame: a background to build on.
    m_production.addSource(QStringLiteral("colour"), nextName(tr("Colour")),
                           {{QStringLiteral("colour"), QStringLiteral("#202830")},
                            {QStringLiteral("width"), canvasWidth()},
                            {QStringLiteral("height"), canvasHeight()}},
                           {});
}

void Backend::replaceImage(const QString &sourceId) {
    m_imageTarget = sourceId;
    m_pickGeneration = m_production.generation();
    openImagePicker();
}

void Backend::addImage() {
    m_imageTarget.clear();
    openImagePicker();
}

void Backend::openImagePicker() {
    m_imagePicker.openFile(tr("Add an image"), tr("Images"),
                           {QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
                            QStringLiteral("*.webp"), QStringLiteral("*.gif"), QStringLiteral("*.apng"), QStringLiteral("*.bmp"),
                            QStringLiteral("*.svg"), QStringLiteral("*.tif"), QStringLiteral("*.tiff")});
}

void Backend::addText() {
    Transform t;
    t.x = canvasWidth() * 0.08;
    t.y = canvasHeight() * 0.08;
    m_production.addSource(QStringLiteral("text"), nextName(tr("Text")),
                           {{QStringLiteral("text"), tr("Text")},
                            {QStringLiteral("size"), std::max(24, canvasHeight() / 12)},
                            {QStringLiteral("colour"), QStringLiteral("#ffffff")}},
                           t);
}

void Backend::addSlideshow() {
    const auto item = m_production.addSource("slideshow", tr("Slideshow"),
        {{"entries", QVariantList{}}, {"interval", 5.}, {"fade", 700}, {"loop", true},
         {"width", canvasWidth()}, {"height", canvasHeight()}}, {});
    setSelectedItem(item);
    if (const auto *entry = m_production.collection().findItem(item)) emit slideshowEditRequested(entry->source);
}

bool Backend::hasSlideshow(const QString &sourceId) const {
    const auto *source = m_production.collection().source(sourceId);
    return source && source->kind == "slideshow";
}

QVariantList Backend::slideshowItems(const QString &sourceId) const {
    const auto *source = m_production.collection().source(sourceId);
    if (!source || source->kind != "slideshow") return {};
    auto entries = slideshowEntries(source->settings);
    for (auto &value : entries) {
        auto entry = value.toMap(); const QFileInfo file(entry.value("path").toString());
        entry["name"] = file.fileName();
        entry["missing"] = entry.value("folder").toBool() ? !file.isDir() : !file.isFile();
        value = entry;
    }
    return entries;
}

bool Backend::setSlideshowEntries(const QString &sourceId, const QVariantList &entries) {
    const auto *source = m_production.collection().source(sourceId);
    if (!source || source->kind != "slideshow") return false;
    auto settings = source->settings;
    settings.remove("folder"); settings.remove("files");
    settings["entries"] = slideshowEntries({{"entries", entries}});
    m_production.setSourcesSettings({{sourceId, settings}}, tr("Edit slideshow images"));
    return true;
}

void Backend::pickSlideshowEntries(const QString &sourceId, bool folder, int replaceIndex) {
    const auto *source = m_production.collection().source(sourceId);
    if (!source || source->kind != "slideshow") return;
    const auto before = slideshowEntries(source->settings);
    if (replaceIndex < -1 || replaceIndex >= before.size()) return;
    const auto generation = m_production.generation();
    m_slideshowTarget = sourceId; m_slideshowGeneration = generation;
    m_slideshowChosen = [this, sourceId, folder, replaceIndex, before, generation](const QList<QUrl> &urls) {
        const auto *source = m_production.collection().source(sourceId);
        if (generation != m_production.generation() || !source || source->kind != "slideshow") return;
        auto entries = slideshowEntries(source->settings);
        if (replaceIndex >= 0 && entries != before) { setStatus(tr("The image list changed. Choose the entry again.")); return; }
        QVariantList added;
        for (const auto &url : urls) {
            const auto path = url.toLocalFile(); const QFileInfo file(path);
            if (!url.isLocalFile() || path.isEmpty() || (folder ? !file.isDir() : !file.isFile() || !QImageReader(path).canRead())) continue;
            added << QVariantMap{{"path", file.absoluteFilePath()}, {"folder", folder}};
        }
        if (added.isEmpty()) { setStatus(tr("No readable local images or folders were selected.")); return; }
        if (replaceIndex >= 0) entries[replaceIndex] = added.first(); else entries += added;
        setSlideshowEntries(sourceId, entries);
    };
    if (folder) m_slideshowPicker.openFolder(tr("Choose a folder of images"));
    else if (replaceIndex >= 0) m_slideshowPicker.openFile(tr("Choose an image"), tr("Images"), {"*.png", "*.jpg", "*.jpeg", "*.webp", "*.gif", "*.svg", "*.bmp", "*.tif", "*.tiff", "*"});
    else m_slideshowPicker.openFiles(tr("Add images to the slideshow"), tr("Images"), {"*.png", "*.jpg", "*.jpeg", "*.webp", "*.gif", "*.svg", "*.bmp", "*.tif", "*.tiff", "*"});
}

// OmaShow draws the slides: `omashow export --stages` gives a picture before
// each click's build and one with the slide complete (an older OmaShow gives
// one a slide). Tests point OMABROADCAST_OMASHOW at their own.
QString Backend::omashowProgram() {
    if (qEnvironmentVariableIsSet("OMABROADCAST_OMASHOW")) return qEnvironmentVariable("OMABROADCAST_OMASHOW");
    return QStandardPaths::findExecutable(QStringLiteral("omashow"));
}

bool Backend::presentationAvailable() { return !omashowProgram().isEmpty(); }

void Backend::addPresentation(const QString &deck) {
    if (!deck.isEmpty()) { loadPresentation(deck, QString()); return; }
    const quint64 generation = m_production.generation();
    m_otherChosen = [this, generation](const QString &path) {
        if (generation == m_production.generation()) loadPresentation(path, QString());
    };
    m_otherPicker.openFile(tr("Choose a presentation"), tr("Presentations"),
                           {QStringLiteral("*.omashow"), QStringLiteral("*.pptx"), QStringLiteral("*.key")});
}

void Backend::reloadPresentation(const QString &sourceId) {
    const auto *source = m_production.collection().source(sourceId);
    if (!source || source->kind != QLatin1String("slideshow")) return;
    const QString deck = source->settings.value(QStringLiteral("presentation")).toString();
    if (!deck.isEmpty()) loadPresentation(deck, sourceId);
}

void Backend::loadPresentation(const QString &deck, const QString &sourceId) {
    const QString program = omashowProgram();
    if (program.isEmpty()) { setStatus(tr("Presentations need OmaShow, which is not installed.")); return; }
    if (m_presentationJob) { setStatus(tr("Still loading a presentation. Try again when it is in.")); return; }
    if (!QFileInfo(deck).isFile()) { setStatus(tr("%1 is not there any more.").arg(QFileInfo(deck).fileName())); return; }
    // A folder of its own each time: the pictures shown now stay until the new ones are in.
    const QString stamp = QString::number(QDateTime::currentMSecsSinceEpoch(), 36);
    const QString folder = presentationsRoot() + QLatin1Char('/') + stamp;
    if (!QDir().mkpath(folder)) { setStatus(tr("No room to load the presentation.")); return; }
    auto *job = new QProcess(this);
    m_presentationJob = job;
    m_presentationLoading = stamp;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    job->setProcessEnvironment(env);
    job->setProcessChannelMode(QProcess::SeparateChannels);
    const QString name = QFileInfo(deck).completeBaseName();
    setStatus(tr("Loading %1…").arg(name));
    const quint64 generation = m_production.generation();
    connect(job, &QProcess::finished, this, [this, job, deck, sourceId, folder, name, generation](int code, QProcess::ExitStatus status) {
        job->deleteLater();
        m_presentationJob = nullptr;
        m_presentationLoading.clear();
        const QJsonObject answer = QJsonDocument::fromJson(job->readAllStandardOutput()).object();
        QVariantList entries;
        for (const QJsonValue &file : answer.value(QStringLiteral("files")).toArray())
            if (QFileInfo(file.toString()).isFile()) entries << QVariantMap{{"path", file.toString()}, {"folder", false}};
        const auto *source = sourceId.isEmpty() ? nullptr : m_production.collection().source(sourceId);
        if (status != QProcess::NormalExit || code != 0 || entries.isEmpty() || generation != m_production.generation() ||
            (!sourceId.isEmpty() && !source)) {
            removePresentationFolder(folder);
            if (generation != m_production.generation() || (!sourceId.isEmpty() && !source)) return;  // nothing to give it to
            const QString why = answer.value(QStringLiteral("error")).toString();
            setStatus(why.isEmpty() ? tr("%1 could not be loaded.").arg(name) : tr("%1 could not be loaded: %2").arg(name, why));
            return;
        }
        if (source) {
            // The pictures it showed stay where they are: Undo, a pasted copy and a
            // duplicated collection still show them. sweepPresentationFolders
            // takes them once nothing does.
            QVariantMap settings = source->settings;
            settings.insert(QStringLiteral("entries"), entries);
            settings.insert(QStringLiteral("presentationFolder"), folder);
            m_production.setSourcesSettings({{sourceId, settings}}, tr("Load the presentation again"));
            setStatus(tr("%1 loaded again.").arg(name));
            return;
        }
        // Stepped by hand from the first picture, with a short fade, and held on the last.
        const auto item = m_production.addSource(QStringLiteral("slideshow"), name,
            {{"entries", entries}, {"manual", true}, {"loop", false}, {"transition", "fade"}, {"fade", 300}, {"interval", 5.},
             {"width", canvasWidth()}, {"height", canvasHeight()},
             {"presentation", QFileInfo(deck).absoluteFilePath()}, {"presentationFolder", folder}}, {});
        setSelectedItem(item);
        setStatus(tr("%1 loaded. Next and Previous step through it.").arg(name));
    });
    connect(job, &QProcess::errorOccurred, this, [this, job, folder](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;  // the rest end in finished
        job->deleteLater();
        m_presentationJob = nullptr;
        m_presentationLoading.clear();
        removePresentationFolder(folder);
        setStatus(tr("OmaShow would not start, so the presentation was not loaded."));
    });
    // A deck that never finishes drawing is given up on, not waited for for ever.
    QTimer::singleShot(180000, job, [job] { job->kill(); });
    job->start(program, {QStringLiteral("export"), deck, QStringLiteral("--kind"), QStringLiteral("images"), QStringLiteral("--stages"),
                         QStringLiteral("--width"), QString::number(std::clamp(canvasWidth(), 160, 7680)),
                         QStringLiteral("--out"), folder + QStringLiteral("/slide.png"), QStringLiteral("--force")});
}

int Backend::sweepPresentationFolders() {
    const QString root = presentationsRoot();
    const QString real = QDir(root).canonicalPath();
    if (real.isEmpty()) return 0;  // none was ever loaded
    const QStringList roots{QDir::cleanPath(root), real};
    QSet<QString> used;
    // Every collection kept on disk, read as it is written. One that does not
    // read may name any of the folders: then nothing goes.
    const QDir collections(QFileInfo(collectionFile(QStringLiteral("x"))).absolutePath());
    for (const QFileInfo &file : collections.entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile f(file.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) return 0;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject()) return 0;
        presentationFoldersNamed(doc.toVariant(), roots, used);
    }
    // And what is held here: the collection open (it may not be saved yet), what
    // Undo and Redo can bring back, and what was copied.
    m_production.forEachKept([&](const Collection &c) {
        for (const SourceDef &source : c.sources) presentationFoldersNamed(source.settings, roots, used);
    });
    int removed = 0;
    for (const QFileInfo &entry : QDir(real).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (used.contains(entry.fileName()) || entry.fileName() == m_presentationLoading) continue;
        if (removePresentationFolder(entry.absoluteFilePath())) ++removed;
    }
    return removed;
}

void Backend::chooseTextFile(const QString &sourceId) {
    const quint64 generation = m_production.generation();
    m_otherChosen = [this, sourceId, generation](const QString &path) {
        if (generation == m_production.generation()) setSourceSetting(sourceId, QStringLiteral("file"), path);
    };
    m_otherPicker.openFile(tr("Show the text in a file"), tr("Text"),
                           {QStringLiteral("*.txt"), QStringLiteral("*.log"), QStringLiteral("*.html"), QStringLiteral("*.htm"), QStringLiteral("*")});
}

void Backend::addDevice(const QString &node) {
    QString name = tr("Capture device");
    for (const VideoDevice &d : m_session.videoDevices())
        if (QString::fromStdString(d.name) == node) name = QString::fromStdString(d.description);
    m_production.addSource(QStringLiteral("device"), name, {{QStringLiteral("node"), node}}, canvasFit());
}

void Backend::addMedia() {
    m_mediaTarget.clear();
    m_mediaPicker.openFile(tr("Add a video or audio file"), tr("Media"),
                           {QStringLiteral("*.mp4"), QStringLiteral("*.mkv"), QStringLiteral("*.mov"),
                            QStringLiteral("*.webm"), QStringLiteral("*.avi"), QStringLiteral("*.ts"),
                            QStringLiteral("*.m4v"), QStringLiteral("*.mp3"), QStringLiteral("*.wav"),
                            QStringLiteral("*.flac"), QStringLiteral("*.ogg"), QStringLiteral("*.m4a")});
}

void Backend::replaceMedia(const QString &sourceId) {
    addMedia();
    m_mediaTarget = sourceId;
    m_pickGeneration = m_production.generation();
}

QString Backend::listenUrl(const QString &protocol, int port, int latencyMs) {
    if (protocol == QLatin1String("rtmp")) return QStringLiteral("rtmp://0.0.0.0:%1/live/omabroadcast").arg(port);
    // SRT's latency is in microseconds.
    return QStringLiteral("srt://0.0.0.0:%1?mode=listener&latency=%2").arg(port).arg(qint64(latencyMs) * 1000);
}

void Backend::addNetworkStream() {
    // The first SRT port no other stream listens on.
    int port = 9000;
    for (bool taken = true; taken; ++port) {
        taken = false;
        for (const SourceDef &s : m_production.collection().sources)
            taken |= s.kind == QLatin1String("media") && s.settings.value(QStringLiteral("port")).toInt() == port;
        if (!taken) break;
    }
    m_production.addSource(QStringLiteral("media"), nextName(tr("Phone")),
                           {{QStringLiteral("input"), listenUrl(QStringLiteral("srt"), port, 120)},
                            {QStringLiteral("protocol"), QStringLiteral("srt")},
                            {QStringLiteral("port"), port},
                            {QStringLiteral("latency"), 120},
                            {QLatin1String(kPassphraseKey), newPassphrase()},
                            {QStringLiteral("volume"), 1.0}},
                           canvasFit());
}

void Backend::setNetworkStream(const QString &sourceId, const QString &protocol, int port, int latencyMs) {
    const SourceDef *src = m_production.collection().source(sourceId);
    if (!src || port < 1 || port > 65535) return;
    QVariantMap s = src->settings;
    s.insert(QStringLiteral("protocol"), protocol);
    s.insert(QStringLiteral("port"), port);
    s.insert(QStringLiteral("latency"), std::max(20, latencyMs));
    s.insert(QStringLiteral("input"), listenUrl(protocol, port, std::max(20, latencyMs)));
    m_production.setSourceSettings(sourceId, s);
}

QString Backend::setNetworkPassphrase(const QString &sourceId, const QString &passphrase) {
    const SourceDef *src = m_production.collection().source(sourceId);
    if (!src || !isNetworkStream(*src)) return {};
    const QString wanted = passphrase.trimmed();
    if (!wanted.isEmpty() && wanted.size() < kPassphraseMin)
        return tr("A passphrase needs at least %1 characters. Leave it empty to turn protection off.").arg(kPassphraseMin);
    if (wanted.size() > kPassphraseMax) return tr("A passphrase can have at most %1 characters.").arg(kPassphraseMax);
    if (!wanted.isEmpty() && !passphraseFits(wanted)) return tr("A passphrase can only use letters, digits and - _ . ~");
    QVariantMap s = src->settings;
    s.insert(QLatin1String(kPassphraseKey), wanted);
    m_production.setSourceSettings(sourceId, s);
    return {};
}

void Backend::newNetworkPassphrase(const QString &sourceId) {
    setNetworkPassphrase(sourceId, newPassphrase());
}

QVariantList Backend::sendAddresses(const QString &protocol, int port, const QString &passphrase) {
    QVariantList out;
    for (const QNetworkInterface &nif : QNetworkInterface::allInterfaces()) {
        const auto flags = nif.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
            (flags & QNetworkInterface::IsLoopBack))
            continue;
        // What the link is, from its driver where the name says nothing.
        const QString name = nif.name();
        const QString driver = QFileInfo(QStringLiteral("/sys/class/net/%1/device/driver").arg(name)).symLinkTarget();
        QString label = name;
        if (driver.endsWith(QLatin1String("/ipheth"))) label = tr("iPhone over USB");
        else if (name.startsWith(QLatin1String("tailscale"))) label = QStringLiteral("Tailscale");
        else if (nif.type() == QNetworkInterface::Wifi || name.startsWith(QLatin1String("wl"))) label = tr("Wi-Fi");
        else if (nif.type() == QNetworkInterface::Ethernet) label = tr("Ethernet");
        else if (nif.type() == QNetworkInterface::Virtual || name.startsWith(QLatin1String("docker")) ||
                 name.startsWith(QLatin1String("veth")) || name.startsWith(QLatin1String("br-")))
            continue;  // containers and bridges: nothing a phone can reach
        for (const QNetworkAddressEntry &e : nif.addressEntries()) {
            if (e.ip().protocol() != QAbstractSocket::IPv4Protocol) continue;
            const bool rtmp = protocol == QLatin1String("rtmp");
            const QString host = QStringLiteral("%1:%2").arg(e.ip().toString()).arg(port);
            QVariantMap address{{QStringLiteral("label"), label}};
            if (passphrase.isEmpty()) {
                address.insert(QStringLiteral("url"), rtmp ? QStringLiteral("rtmp://%1/live/%2").arg(host, QLatin1String(kRtmpStreamName))
                                                           : QStringLiteral("srt://%1").arg(host));
            } else if (rtmp) {
                address.insert(QStringLiteral("url"), QStringLiteral("rtmp://%1/%2").arg(host, passphrase));
                address.insert(QStringLiteral("streamKey"), QLatin1String(kRtmpStreamName));
                address.insert(QStringLiteral("inAddress"), QStringLiteral("rtmp://%1/%2/%3").arg(host, passphrase, QLatin1String(kRtmpStreamName)));
            } else {
                address.insert(QStringLiteral("url"), QStringLiteral("srt://%1").arg(host));
                address.insert(QStringLiteral("inAddress"), QStringLiteral("srt://%1?passphrase=%2").arg(host, passphrase));
            }
            out.append(address);
        }
    }
    return out;
}

QString Backend::firewallHint(const QString &protocol, int port, const QString &ufwDir) {
    QFile conf(ufwDir + QStringLiteral("/ufw.conf"));
    if (!conf.open(QIODevice::ReadOnly) || !QString::fromUtf8(conf.readAll()).contains(QLatin1String("ENABLED=yes")))
        return {};
    // SRT is UDP, RTMP is TCP; a rule for the port on that protocol, or on
    // any protocol, lets it through. (Rules narrower than that are not read.)
    const QString proto = protocol == QLatin1String("rtmp") ? QStringLiteral("tcp") : QStringLiteral("udp");
    QFile rules(ufwDir + QStringLiteral("/user.rules"));
    if (rules.open(QIODevice::ReadOnly)) {
        const QRegularExpression allowed(QStringLiteral("^### tuple ### allow (%1|any) %2 ").arg(proto).arg(port),
                                         QRegularExpression::MultilineOption);
        if (allowed.match(QString::fromUtf8(rules.readAll())).hasMatch()) return {};
    }
    return QStringLiteral("sudo ufw allow %1/%2").arg(port).arg(proto);
}

QVariantList Backend::deviceControlList(const QString &sourceId) const {
    QVariantList out;
    for (const DeviceControl &c : m_session.deviceControls(sourceId)) {
        QVariantList menu;
        for (const auto &[value, label] : c.menu)
            menu.append(QVariantMap{{QStringLiteral("value"), value}, {QStringLiteral("label"), QString::fromStdString(label)}});
        out.append(QVariantMap{{QStringLiteral("id"), int(c.id)},
                               {QStringLiteral("name"), QString::fromStdString(c.name)},
                               {QStringLiteral("type"), c.type == DeviceControl::Type::Boolean ? QStringLiteral("bool")
                                                        : c.type == DeviceControl::Type::Menu  ? QStringLiteral("menu")
                                                                                               : QStringLiteral("int")},
                               {QStringLiteral("min"), c.minimum},
                               {QStringLiteral("max"), c.maximum},
                               {QStringLiteral("step"), c.step},
                               {QStringLiteral("default"), c.defaultValue},
                               {QStringLiteral("value"), c.value},
                               {QStringLiteral("inactive"), c.inactive},
                               {QStringLiteral("menu"), menu}});
    }
    return out;
}

void Backend::setDeviceControl(const QString &sourceId, int controlId, int value, bool gesture) {
    const SourceDef *src = m_production.collection().source(sourceId);
    if (!src) return;
    QVariantMap controls = src->settings.value(QStringLiteral("controls")).toMap();
    controls.insert(QString::number(controlId), value);
    setSourceSetting(sourceId, QStringLiteral("controls"), controls, gesture);
}

void Backend::resetDeviceControls(const QString &sourceId) {
    // Every control back to the device's own default, saved like any change.
    QVariantMap controls;
    for (const DeviceControl &c : m_session.deviceControls(sourceId))
        controls.insert(QString::number(c.id), c.defaultValue);
    setSourceSetting(sourceId, QStringLiteral("controls"), controls);
}

bool Backend::setCanvasSize(int width, int height) {
    width = std::clamp(width & ~1, 16, 8192);
    height = std::clamp(height & ~1, 16, 8192);
    if (outputsBusy()) {
        setStatus(tr("The canvas size can change once recording, streaming, the replay buffer and the virtual camera stop."));
        return false;
    }
    if (canvasWidth() == width && canvasHeight() == height) return true;
    fitLayoutToCanvas();  // a layout that does not say what size it is at: this one
    m_production.followCanvas(width, height);
    m_session.setCanvasSize(width, height);
    m_session.profile().canvasWidth = width;
    m_session.profile().canvasHeight = height;
    saveProfile();
    emit settingsChanged();
    return true;
}

QVariantList Backend::videoDevices() const {
    QVariantList out;
    for (const VideoDevice &d : m_session.videoDevices())
        out.append(QVariantMap{{QStringLiteral("name"), QString::fromStdString(d.name)},
                               {QStringLiteral("description"), QString::fromStdString(d.description)}});
    return out;
}

void Backend::removeItem(const QString &id) { m_production.removeItem(id); }

void Backend::removeSelectedItems() {
    QStringList ids = selectedItems();
    if (ids.isEmpty() && !selectedItem().isEmpty()) ids << selectedItem();
    if (!ids.isEmpty()) m_production.removeItems(ids);
}

void Backend::dropItem(const QString &id, const QString &targetId, bool below) {
    if (id == targetId) return;
    const Collection &c = m_production.collection();
    const Scene *from = nullptr, *to = nullptr;
    if (!c.findItem(id, &from) || !c.findItem(targetId, &to)) return;
    if (from != to) {
        setStatus(tr("Use Group and Ungroup to move sources in or out of a group."));
        return;
    }
    // The stack runs bottom to top; the list, top to bottom.
    QStringList order;
    for (const SceneItem &i : from->items)
        if (i.id != id) order << i.id;
    const int t = int(order.indexOf(targetId));
    m_production.moveItem(id, below ? t : t + 1);
}

void Backend::copyPlacement(const QString &id) {
    const SceneItem *item = m_production.collection().findItem(id);
    if (!item) return;
    m_placement = item->transform;
    m_hasPlacement = true;
    emit placementCopied();
}

void Backend::copySources() {
    if (!m_production.copyItems(selectedItems()))
        setStatus(tr("Select sources side by side in one studio or group to copy them."));
}

void Backend::pasteSources(bool duplicate) {
    if (m_production.pasteItems(duplicate).isEmpty())
        setStatus(tr("These sources cannot be pasted here. Copy them again, or paste an independent duplicate."));
}

void Backend::pastePlacement() {
    if (!m_hasPlacement) return;
    QHash<QString, Transform> placed;
    for (const QString &id : selectedItems())
        if (const SceneItem *item = m_production.collection().findItem(id); item && !placementLocked(id)) placed.insert(id, m_placement);
    if (!placed.isEmpty()) m_production.setTransforms(placed);
}

void Backend::addStudio(const QString &studioId) {
    if (m_production.addStudioSource(studioId, canvasFit()).isEmpty())
        setStatus(tr("A studio cannot show itself, or a studio that shows it."));
}

void Backend::groupSelected() {
    const QStringList ids = selectedItems();
    if (ids.isEmpty()) return;
    // One box around them all, in their own studio's or group's coordinates.
    const Collection &c = m_production.collection();
    const Scene *owner = nullptr;
    c.findItem(ids.first(), &owner);
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const QString &id : ids) {
        const Scene *o = nullptr;
        const SceneItem *item = c.findItem(id, &o);
        if (!item || o != owner) {
            setStatus(tr("Only items side by side in one studio or group can be grouped."));
            return;
        }
        const QSize size = sourceSize(item->source);
        const Outline ol = itemOutline(item->transform, size.width(), size.height());
        const double r = ol.rotation * M_PI / 180.0;
        const double ex = std::abs(std::cos(r)) * ol.width / 2 + std::abs(std::sin(r)) * ol.height / 2;
        const double ey = std::abs(std::sin(r)) * ol.width / 2 + std::abs(std::cos(r)) * ol.height / 2;
        const double cx = ol.x + ol.width / 2, cy = ol.y + ol.height / 2;
        x0 = std::min(x0, cx - ex);
        y0 = std::min(y0, cy - ey);
        x1 = std::max(x1, cx + ex);
        y1 = std::max(y1, cy + ey);
    }
    m_production.groupItems(ids, QRectF(std::floor(x0), std::floor(y0), std::ceil(x1) - std::floor(x0),
                                        std::ceil(y1) - std::floor(y0)));
}

void Backend::ungroupSelected() {
    m_production.ungroupItems(selectedItems(), [this](const QString &sourceId) { return QSizeF(sourceSize(sourceId)); });
}
void Backend::setItemVisible(const QString &id, bool visible) { m_production.setItemVisible(id, visible); }
void Backend::setItemTransition(const QString &id, bool show, const QString &transition, int ms) {
    m_production.setItemTransition(id, show, transition, ms);
}

void Backend::setItemLocked(const QString &id, bool locked) { m_production.setItemLocked(id, locked); }

void Backend::raiseItem(const QString &id, int steps) {
    const Scene *scene = nullptr;  // its studio, or its group
    m_production.collection().findItem(id, &scene);
    if (!scene) return;
    for (int i = 0; i < scene->items.size(); ++i)
        if (scene->items[i].id == id) {
            m_production.moveItem(id, i + steps);
            return;
        }
}

void Backend::renameSource(const QString &sourceId, const QString &name) {
    if (!name.trimmed().isEmpty()) m_production.renameSource(sourceId, name.trimmed());
}

void Backend::reopenSource(const QString &sourceId) {
    const auto *source=m_production.collection().source(sourceId);
    if (source && source->settings.value("captureMethod")=="direct") chooseDirectCapture(sourceId);
    else m_session.reopenFeed(sourceId);
}

void Backend::addDirectCapture(bool window, bool focused) {
    const auto item=m_production.addSource(window ? "window" : "display", nextName(focused ? tr("Focused window") : window ? tr("Window") : tr("Display")),
        {{"captureMethod","direct"},{"cursor",true},{"followFocus",window && focused}},canvasFit());
    if (!focused) if (const auto *entry=m_production.collection().findItem(item)) chooseDirectCapture(entry->source);
}

void Backend::chooseDirectCapture(const QString &sourceId) {
    const auto *source=m_production.collection().source(sourceId);
    if (source && (source->kind=="display" || source->kind=="window" || source->kind=="region"))
        emit directCaptureRequested(sourceId,source->kind=="window");
}

QVariantMap Backend::directCaptureCatalog() {
    const auto catalog=m_session.directCaptureCatalog(); QVariantList targets;
    for (const auto &target:catalog.targets) targets << QVariantMap{{"key",QString::fromStdString(target.key)},
        {"name",QString::fromStdString(target.name)},{"appId",QString::fromStdString(target.appId)},
        {"title",QString::fromStdString(target.title)},{"window",target.window},{"focused",target.focused}};
    return {{"ready",catalog.ready},{"displays",catalog.displays},{"windows",catalog.windows},
        {"error",QString::fromStdString(catalog.error)},{"targets",targets}};
}

QString Backend::captureSourceKind(const QString &sourceId) const {
    const auto *source=m_production.collection().source(sourceId);
    return source && (source->kind=="window" || source->kind=="display" || source->kind=="region") ? source->kind : QString();
}

bool Backend::setDirectCaptureTarget(const QString &sourceId, const QVariantMap &target, bool focused) {
    const auto *source=m_production.collection().source(sourceId);
    if (!source || (source->kind!="display" && source->kind!="window" && source->kind!="region")) return false;
    const bool window=source->kind=="window";
    if ((focused && !window) || (!focused && (target.value("key").toString().isEmpty() || target.value("window").toBool()!=window))) return false;
    auto chosen=target;
    if (!focused) {
        const auto catalog=m_session.directCaptureCatalog();
        const auto found=std::find_if(catalog.targets.begin(),catalog.targets.end(),[&](const auto &entry) {
            return entry.window==window && QString::fromStdString(entry.key)==target.value("key").toString();
        });
        if (found==catalog.targets.end()) return false;
        chosen["appId"]=QString::fromStdString(found->appId); chosen["title"]=QString::fromStdString(found->title);
    }
    auto settings=source->settings; settings["captureMethod"]="direct"; settings["followFocus"]=focused;
    for (const auto &key:{"directKey","directOutput","directApp","directTitle"}) settings.remove(key);
    if (!focused) {
        settings["directKey"]=chosen.value("key");
        if (window) { settings["directApp"]=chosen.value("appId"); settings["directTitle"]=chosen.value("title"); }
        else settings["directOutput"]=chosen.value("key").toString().mid(7);
    }
    m_production.setSourceSettings(sourceId,settings);
    return true;
}

void Backend::usePortalCapture(const QString &sourceId) {
    const auto *source=m_production.collection().source(sourceId);
    if (!source || (source->kind!="display" && source->kind!="window" && source->kind!="region")) return;
    auto settings=source->settings; settings["captureMethod"]="portal";
    if (source->settings==settings) m_session.reopenFeed(sourceId);
    else m_production.setSourceSettings(sourceId,settings);
}

void Backend::setSourceSetting(const QString &sourceId, const QString &key, const QVariant &value, bool gesture) {
    const SourceDef *src = m_production.collection().source(sourceId);
    if (!src) return;
    QVariantMap settings = src->settings;
    settings.insert(key, value);
    if (src->hasAudio()) {
        if (key == QLatin1String("volume")) settings.insert(key, AudioSettings::gain(value.toDouble()));
        else if (AudioSettings::isKey(key))
            settings.insert(key, AudioSettings::normalized(settings).value(key));
    }
    if (key == QLatin1String("audioNode") && (SourceDef::isAudioKind(src->kind) || src->kind == QLatin1String("device"))) {
        const QString kind = src->kind == QLatin1String("device") ? QStringLiteral("audio-input") : src->kind;
        if (value.toString().isEmpty()) settings.remove(QStringLiteral("audioLabel"));
        for (const QVariant &entry : audioSourceTargets()) {
            const QVariantMap row = entry.toMap();
            if (row.value(QStringLiteral("kind")) == kind && row.value(QStringLiteral("value")) == value)
                settings.insert(QStringLiteral("audioLabel"), row.value(QStringLiteral("label")));
        }
    }
    // Another device has other modes: it starts at its best, in the same step.
    if (key == QLatin1String("node") && src->kind == QLatin1String("device") && value != src->settings.value(key)) settings.remove(QStringLiteral("mode"));
    const QString name = src->name;
    const bool deviceMode = key == QLatin1String("mode") && src->kind == QLatin1String("device") && !value.toString().isEmpty();
    m_production.setSourceSettings(sourceId, settings, !m_settingMergeKey.isEmpty() ? m_settingMergeKey
                                                      : gesture ? QStringLiteral("setting:") + sourceId + key : QString());
    // A mode at another frame rate is the rate wanted: the canvas, and with
    // it every recording and stream, takes it rather than having to be set
    // to it in Settings as well.
    if (deviceMode) {
        for (const VideoDevice &d : m_session.videoDevices())
            for (const VideoMode &m : d.modes)
                if (QString::fromStdString(m.key()) == value.toString() && m.fpsNum > 0 && (m.fpsNum != fpsNum() || m.fpsDen != fpsDen())) {
                    applyFrameRate(m.fpsNum, m.fpsDen, name);
                    return;
                }
    }
    // A cursor change only reaches a capture through a new portal session.
    if (key == QLatin1String("cursor")) m_session.reopenFeed(sourceId);
}

// ------------------------------------------------------------- transforms

void Backend::changeTransform(const QString &id, bool gesture,
                              const std::function<void(Transform &, int, int)> &change) {
    if (placementLocked(id)) return;
    Transform t;
    int w = 0, h = 0;
    if (!itemState(id, &t, &w, &h) || w <= 0 || h <= 0) return;
    change(t, w, h);
    m_production.setTransform(id, t, gesture ? QStringLiteral("transform:") + id : QString());
}

void Backend::setItemBox(const QString &id, qreal x, qreal y, qreal width, qreal height, bool gesture) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height)) return;
    const auto canvas = canvasOutline(id);
    if (canvas.width <= 0 || canvas.height <= 0) return;
    width = std::max<qreal>(1, width);
    height = std::max<qreal>(1, height);
    QPointF centre(x + width / 2, y + height / 2);
    const auto steps = groupSteps(id);
    for (auto st = steps.crbegin(); st != steps.crend(); ++st) centre = downThrough(*st, centre);
    changeTransform(id, gesture, [&](Transform &t, int w, int h) {
        const auto local = itemOutline(t, w, h);
        const double lw = width * local.width / canvas.width, lh = height * local.height / canvas.height;
        ::setItemBox(t, w, h, centre.x() - lw / 2, centre.y() - lh / 2, lw, lh);
    });
}

void Backend::setItemBoxes(const QVariantList &boxes, bool gesture) {
    QHash<QString, Transform> moved;
    for (const QVariant &v : boxes) {
        const QVariantMap b = v.toMap();
        const QString id = b.value(QStringLiteral("id")).toString();
        const SceneItem *item = m_production.collection().findItem(id);
        if (!item || placementLocked(id)) continue;
        Outline o{b.value(QStringLiteral("x")).toDouble(), b.value(QStringLiteral("y")).toDouble(),
                  b.value(QStringLiteral("width")).toDouble(), b.value(QStringLiteral("height")).toDouble(),
                  canvasOutline(id).rotation};
        const auto canvas = canvasOutline(id);
        if (canvas.width <= 0 || canvas.height <= 0 || !std::isfinite(o.x) || !std::isfinite(o.y) ||
            !std::isfinite(o.width) || !std::isfinite(o.height)) continue;
        QPointF centre(o.x + o.width / 2, o.y + o.height / 2);
        const auto steps = groupSteps(id);
        for (auto st = steps.crbegin(); st != steps.crend(); ++st) centre = downThrough(*st, centre);
        Transform t = item->transform;
        const QSize size = sourceSize(item->source);
        const auto local = itemOutline(t, size.width(), size.height());
        const double lw = std::max(1.0, o.width) * local.width / canvas.width;
        const double lh = std::max(1.0, o.height) * local.height / canvas.height;
        ::setItemBox(t, size.width(), size.height(), centre.x() - lw / 2, centre.y() - lh / 2, lw, lh);
        moved.insert(id, t);
    }
    if (!moved.isEmpty()) m_production.setTransforms(moved, gesture ? QStringLiteral("boxes") : QString());
}

void Backend::setItemRotation(const QString &id, qreal degrees, bool gesture) {
    if (!std::isfinite(degrees)) return;
    const auto steps = groupSteps(id);
    for (auto st = steps.crbegin(); st != steps.crend(); ++st)
        degrees = mapOutline(*st, {0, 0, 1, 1, degrees}, false).rotation;
    changeTransform(id, gesture, [&](Transform &t, int, int) {
        double d = std::fmod(degrees, 360.0);
        if (d < 0) d += 360.0;
        t.rotation = d;
    });
}

void Backend::setItemCrop(const QString &id, int left, int top, int right, int bottom, bool gesture) {
    changeTransform(id, gesture, [&](Transform &t, int w, int h) {
        // Never crop the whole source away.
        t.cropLeft = std::clamp(left, 0, w - 1);
        t.cropRight = std::clamp(right, 0, w - 1 - t.cropLeft);
        t.cropTop = std::clamp(top, 0, h - 1);
        t.cropBottom = std::clamp(bottom, 0, h - 1 - t.cropTop);
    });
}

void Backend::setItemCropAt(const QString &id, int left, int top, int right, int bottom, qreal x, qreal y,
                            bool gesture) {
    changeTransform(id, gesture, [&](Transform &t, int w, int h) {
        t.cropLeft = std::clamp(left, 0, w - 1);
        t.cropRight = std::clamp(right, 0, w - 1 - t.cropLeft);
        t.cropTop = std::clamp(top, 0, h - 1);
        t.cropBottom = std::clamp(bottom, 0, h - 1 - t.cropTop);
        const Outline local = itemOutline(t, w, h);
        Outline canvas = local;
        const auto steps = groupSteps(id);
        for (const auto &st : steps) canvas = mapOutline(st, canvas, true);
        QPointF centre(x + canvas.width / 2, y + canvas.height / 2);
        for (auto st = steps.crbegin(); st != steps.crend(); ++st) centre = downThrough(*st, centre);
        ::setItemBox(t, w, h, centre.x() - local.width / 2, centre.y() - local.height / 2, local.width, local.height);
    });
}

void Backend::setItemFlip(const QString &id, bool horizontal, bool vertical) {
    changeTransform(id, false, [&](Transform &t, int, int) {
        t.flipH = horizontal;
        t.flipV = vertical;
    });
}

void Backend::setItemBounds(const QString &id, const QString &bounds) {
    changeTransform(id, false, [&](Transform &t, int w, int h) {
        // Keep the outline where it is while its meaning changes.
        const Outline o = itemOutline(t, w, h);
        t.bounds = bounds == QLatin1String("fit")        ? Transform::Bounds::Fit
                   : bounds == QLatin1String("fill")     ? Transform::Bounds::Fill
                   : bounds == QLatin1String("stretch")  ? Transform::Bounds::Stretch
                   : bounds == QLatin1String("cover")    ? Transform::Bounds::Cover
                   : bounds == QLatin1String("width")    ? Transform::Bounds::Width
                   : bounds == QLatin1String("height")   ? Transform::Bounds::Height
                   : bounds == QLatin1String("max-only") ? Transform::Bounds::MaxOnly
                                                         : Transform::Bounds::None;
        ::setItemBox(t, w, h, o.x, o.y, o.width, o.height);
    });
}

void Backend::setItemPosition(const QString &id, qreal x, qreal y) {
    // The anchor, in canvas coordinates.
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    QPointF p(x, y);
    const auto steps = groupSteps(id);
    for (auto st = steps.crbegin(); st != steps.crend(); ++st) p = downThrough(*st, p);
    changeTransform(id, false, [&](Transform &t, int, int) {
        t.x = p.x();
        t.y = p.y();
    });
}

void Backend::rotateItem(const QString &id, qreal degrees) {
    setItemRotation(id, canvasOutline(id).rotation + degrees);
}

void Backend::editTransform(const QString &id) {
    const SceneItem *item = m_production.collection().findItem(id);
    const SourceDef *source = item ? m_production.collection().source(item->source) : nullptr;
    if (!source || SourceDef::isAudioKind(source->kind)) return;
    setSelectedItem(id);
    emit transformEditRequested(id);
}

void Backend::setItemSize(const QString &id, qreal width, qreal height) {
    // As it shows on the canvas, keeping the anchor where it is.
    if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1) return;
    const Outline canvas = canvasOutline(id);
    if (canvas.width <= 0 || canvas.height <= 0) return;
    changeTransform(id, false, [&](Transform &t, int w, int h) {
        const Outline o = itemOutline(t, w, h);
        const double x = t.x, y = t.y;
        ::setItemBox(t, w, h, o.x, o.y, width * o.width / canvas.width, height * o.height / canvas.height);
        t.x = x;
        t.y = y;
    });
}

void Backend::setItemAlignment(const QString &id, const QString &kind, int alignment) {
    if (alignment < 0 || alignment > 8) return;
    if (kind != QLatin1String("position") && kind != QLatin1String("rotation") && kind != QLatin1String("bounds")) return;
    changeTransform(id, false, [&](Transform &t, int w, int h) {
        if (kind == QLatin1String("bounds")) {
            t.boundsAlignment = Transform::Alignment(alignment);
            return;
        }
        // A new anchor or pivot leaves the outline where it shows.
        const Outline o = itemOutline(t, w, h);
        if (kind == QLatin1String("position")) t.positionAlignment = Transform::Alignment(alignment);
        else t.rotationAlignment = Transform::Alignment(alignment);
        ::setItemBox(t, w, h, o.x, o.y, o.width, o.height);
    });
}

void Backend::setItemClipToBounds(const QString &id, bool clip) {
    changeTransform(id, false, [&](Transform &t, int, int) { t.clipToBounds = clip; });
}

void Backend::setItemScaleFilter(const QString &id, const QString &filter) {
    changeTransform(id, false, [&](Transform &t, int, int) {
        t.scale = filter == QLatin1String("bilinear") ? engine::ScaleFilter::Bilinear
                  : filter == QLatin1String("bicubic") ? engine::ScaleFilter::Bicubic
                  : filter == QLatin1String("lanczos") ? engine::ScaleFilter::Lanczos
                  : filter == QLatin1String("area")    ? engine::ScaleFilter::Area
                  : filter == QLatin1String("point")   ? engine::ScaleFilter::Point
                                                        : engine::ScaleFilter::Auto;
    });
}

void Backend::setItemOpacity(const QString &id, qreal opacity, bool gesture) {
    changeTransform(id, gesture, [&](Transform &t, int, int) { t.opacity = std::clamp<double>(opacity, 0, 1); });
}

void Backend::setItemBlend(const QString &id, const QString &blend) {
    static const QStringList names{QStringLiteral("normal"), QStringLiteral("multiply"), QStringLiteral("screen"),
                                   QStringLiteral("overlay"), QStringLiteral("darken"), QStringLiteral("lighten"),
                                   QStringLiteral("add"), QStringLiteral("subtract"), QStringLiteral("difference")};
    const int i = names.indexOf(blend);
    changeTransform(id, false, [&](Transform &t, int, int) { t.blend = engine::BlendMode(std::max(0, i)); });
}

void Backend::setItemBlendSpace(const QString &id, const QString &space) {
    changeTransform(id, false, [&](Transform &t, int, int) {
        t.blendSpace = space == QLatin1String("srgb") ? engine::BlendSpace::Srgb : engine::BlendSpace::Linear;
    });
}

void Backend::placeOnCanvas(const QString &id, bool level, const std::function<Outline(const Outline &)> &target) {
    // In canvas coordinates: an item in a group is levelled and placed as it
    // shows, then mapped back into the group.
    const auto steps = groupSteps(id);
    double degrees = 0;
    for (auto st = steps.crbegin(); st != steps.crend(); ++st)
        degrees = mapOutline(*st, {0, 0, 1, 1, degrees}, false).rotation;
    changeTransform(id, false, [&](Transform &t, int w, int h) {
        if (level) t.rotation = std::fmod(std::fmod(degrees, 360.0) + 360.0, 360.0);
        const Outline local = itemOutline(t, w, h);
        Outline canvas = local;
        for (const GroupStep &st : steps) canvas = mapOutline(st, canvas, true);
        if (canvas.width <= 0 || canvas.height <= 0) return;
        const Outline box = target(canvas);
        QPointF centre(box.x + box.width / 2, box.y + box.height / 2);
        for (auto st = steps.crbegin(); st != steps.crend(); ++st) centre = downThrough(*st, centre);
        const double lw = std::max(1.0, box.width) * local.width / canvas.width;
        const double lh = std::max(1.0, box.height) * local.height / canvas.height;
        ::setItemBox(t, w, h, centre.x() - lw / 2, centre.y() - lh / 2, lw, lh);
    });
}

void Backend::fitItem(const QString &id) {
    placeOnCanvas(id, true, [&](const Outline &o) {
        const double s = std::min(canvasWidth() / o.width, canvasHeight() / o.height);
        const double fw = o.width * s, fh = o.height * s;
        return Outline{(canvasWidth() - fw) / 2, (canvasHeight() - fh) / 2, fw, fh, 0};
    });
}

void Backend::stretchItem(const QString &id) {
    placeOnCanvas(id, true, [&](const Outline &) {
        return Outline{0, 0, double(canvasWidth()), double(canvasHeight()), 0};
    });
}

void Backend::centreItem(const QString &id) {
    placeOnCanvas(id, false, [&](const Outline &o) {
        return Outline{(canvasWidth() - o.width) / 2, (canvasHeight() - o.height) / 2, o.width, o.height, o.rotation};
    });
}

void Backend::resetItem(const QString &id) {
    changeTransform(id, false, [&](Transform &t, int, int) { t = Transform{}; });
}
