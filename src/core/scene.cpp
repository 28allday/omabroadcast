#include "scene.h"

#include <QJsonArray>
#include <QRandomGenerator>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

const char *scaleName(engine::ScaleFilter f) {
    switch (f) {
    case engine::ScaleFilter::Bilinear: return "bilinear";
    case engine::ScaleFilter::Bicubic: return "bicubic";
    case engine::ScaleFilter::Lanczos: return "lanczos";
    case engine::ScaleFilter::Area: return "area";
    case engine::ScaleFilter::Point: return "point";
    case engine::ScaleFilter::Auto: break;
    }
    return "auto";
}

engine::ScaleFilter scaleFrom(const QString &s) {
    if (s == QLatin1String("point")) return engine::ScaleFilter::Point;
    if (s == QLatin1String("bilinear")) return engine::ScaleFilter::Bilinear;
    if (s == QLatin1String("bicubic")) return engine::ScaleFilter::Bicubic;
    if (s == QLatin1String("lanczos")) return engine::ScaleFilter::Lanczos;
    if (s == QLatin1String("area")) return engine::ScaleFilter::Area;
    return engine::ScaleFilter::Auto;
}

const char *const kBlendNames[] = {"normal", "multiply", "screen", "overlay", "darken",
                                   "lighten", "add", "subtract", "difference"};

engine::BlendMode blendFrom(const QString &s) {
    for (int i = 0; i < 9; ++i)
        if (s == QLatin1String(kBlendNames[i])) return engine::BlendMode(i);
    return engine::BlendMode::Normal;
}

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

Transform::Bounds boundsFrom(const QString &s) {
    if (s == QLatin1String("stretch")) return Transform::Bounds::Stretch;
    if (s == QLatin1String("fit")) return Transform::Bounds::Fit;
    if (s == QLatin1String("fill")) return Transform::Bounds::Fill;
    if (s == QLatin1String("cover")) return Transform::Bounds::Cover;
    if (s == QLatin1String("width")) return Transform::Bounds::Width;
    if (s == QLatin1String("height")) return Transform::Bounds::Height;
    if (s == QLatin1String("max-only")) return Transform::Bounds::MaxOnly;
    return Transform::Bounds::None;
}

const char *const kAlignmentNames[] = {"top-left",    "top",    "top-right",   "left",        "centre",
                                       "right",       "bottom-left", "bottom", "bottom-right"};

Transform::Alignment alignmentFrom(const QJsonValue &value, Transform::Alignment fallback) {
    const QString name = value.toString();
    for (int n = 0; n < 9; ++n)
        if (name == QLatin1String(kAlignmentNames[n])) return Transform::Alignment(n);
    return fallback;
}
QJsonObject toJson(const Transform &t) {
    QJsonObject o{{QStringLiteral("x"), t.x}, {QStringLiteral("y"), t.y},
                  {QStringLiteral("scaleX"), t.scaleX}, {QStringLiteral("scaleY"), t.scaleY},
                  {QStringLiteral("rotation"), t.rotation},
                  {QStringLiteral("crop"), QJsonArray{t.cropLeft, t.cropTop, t.cropRight, t.cropBottom}},
                  {QStringLiteral("flipH"), t.flipH}, {QStringLiteral("flipV"), t.flipV},
                  {QStringLiteral("bounds"), QLatin1String(boundsName(t.bounds))},
                  {QStringLiteral("boundsWidth"), t.boundsWidth}, {QStringLiteral("boundsHeight"), t.boundsHeight},
                  {QStringLiteral("positionAlignment"), QLatin1String(kAlignmentNames[int(t.positionAlignment)])},
                  {QStringLiteral("rotationAlignment"), QLatin1String(kAlignmentNames[int(t.rotationAlignment)])},
                  {QStringLiteral("boundsAlignment"), QLatin1String(kAlignmentNames[int(t.boundsAlignment)])},
                  {QStringLiteral("clipToBounds"), t.clipToBounds},
                  {QStringLiteral("scale"), QLatin1String(scaleName(t.scale))},
                  {QStringLiteral("opacity"), t.opacity},
                  {QStringLiteral("blend"), QLatin1String(kBlendNames[int(t.blend)])},
                  {QStringLiteral("blendSpace"), t.blendSpace == engine::BlendSpace::Srgb ? QStringLiteral("srgb") : QStringLiteral("linear")}};
    return o;
}

Transform transformFrom(const QJsonObject &o) {
    Transform t;
    t.x = o.value(QStringLiteral("x")).toDouble();
    t.y = o.value(QStringLiteral("y")).toDouble();
    t.scaleX = o.value(QStringLiteral("scaleX")).toDouble(1);
    t.scaleY = o.value(QStringLiteral("scaleY")).toDouble(1);
    t.rotation = o.value(QStringLiteral("rotation")).toDouble();
    const QJsonArray crop = o.value(QStringLiteral("crop")).toArray();
    if (crop.size() == 4) {
        // Fitted to the source's size where it is drawn (cropWithin).
        t.cropLeft = std::clamp(crop[0].toInt(), 0, 16384);
        t.cropTop = std::clamp(crop[1].toInt(), 0, 16384);
        t.cropRight = std::clamp(crop[2].toInt(), 0, 16384);
        t.cropBottom = std::clamp(crop[3].toInt(), 0, 16384);
    }
    t.flipH = o.value(QStringLiteral("flipH")).toBool();
    t.flipV = o.value(QStringLiteral("flipV")).toBool();
    t.bounds = boundsFrom(o.value(QStringLiteral("bounds")).toString());
    t.boundsWidth = o.value(QStringLiteral("boundsWidth")).toDouble();
    t.boundsHeight = o.value(QStringLiteral("boundsHeight")).toDouble();
    t.positionAlignment = alignmentFrom(o.value(QStringLiteral("positionAlignment")), Transform::Alignment::TopLeft);
    t.rotationAlignment = alignmentFrom(o.value(QStringLiteral("rotationAlignment")), Transform::Alignment::Centre);
    t.boundsAlignment = alignmentFrom(o.value(QStringLiteral("boundsAlignment")), Transform::Alignment::Centre);
    t.clipToBounds = o.value(QStringLiteral("clipToBounds")).toBool();
    t.scale = scaleFrom(o.value(QStringLiteral("scale")).toString());
    t.opacity = std::clamp(o.value(QStringLiteral("opacity")).toDouble(1), 0.0, 1.0);
    t.blend = blendFrom(o.value(QStringLiteral("blend")).toString());
    t.blendSpace = o.value(QStringLiteral("blendSpace")).toString() == QLatin1String("srgb")
        ? engine::BlendSpace::Srgb : engine::BlendSpace::Linear;
    return t;
}

// The crop as it can apply to a source this size: never negative, and a
// pixel left at least (a source can shrink under a crop made for it bigger).
struct Crop { int left, top, right, bottom; };
Crop cropWithin(const Transform &t, int w, int h) {
    Crop c;
    c.left = std::clamp(t.cropLeft, 0, std::max(0, w - 1));
    c.right = std::clamp(t.cropRight, 0, std::max(0, w - 1 - c.left));
    c.top = std::clamp(t.cropTop, 0, std::max(0, h - 1));
    c.bottom = std::clamp(t.cropBottom, 0, std::max(0, h - 1 - c.top));
    return c;
}

// The source's visible size after cropping; never below a pixel.
void croppedSize(const Transform &t, int w, int h, double *cw, double *ch) {
    const Crop c = cropWithin(t, w, h);
    *cw = std::max(1, w - c.left - c.right);
    *ch = std::max(1, h - c.top - c.bottom);
}

}  // namespace

const SceneItem *Scene::item(const QString &itemId) const {
    for (const SceneItem &i : items)
        if (i.id == itemId) return &i;
    return nullptr;
}
// The non-const lookups walk the containers as non-const so Qt detaches them:
// a const_cast over the shared data would edit every copy at once, and an
// edit to a copy (how Production records history) would change the original.
SceneItem *Scene::item(const QString &itemId) {
    for (SceneItem &i : items)
        if (i.id == itemId) return &i;
    return nullptr;
}

const SourceDef *Collection::source(const QString &id) const {
    for (const SourceDef &s : sources)
        if (s.id == id) return &s;
    return nullptr;
}
SourceDef *Collection::source(const QString &id) {
    for (SourceDef &s : sources)
        if (s.id == id) return &s;
    return nullptr;
}

const Scene *Collection::scene(const QString &id) const {
    for (const Scene &s : scenes)
        if (s.id == id) return &s;
    return nullptr;
}
Scene *Collection::scene(const QString &id) {
    for (Scene &s : scenes)
        if (s.id == id) return &s;
    return nullptr;
}

QJsonObject Collection::toJson() const {
    QJsonArray srcs;
    for (const SourceDef &s : sources)
        srcs.append(QJsonObject{{QStringLiteral("id"), s.id}, {QStringLiteral("kind"), s.kind},
                                {QStringLiteral("name"), s.name},
                                {QStringLiteral("settings"), QJsonObject::fromVariantMap(s.settings)}});
    QJsonArray scns;
    for (const Scene &sc : scenes) {
        QJsonArray items;
        for (const SceneItem &i : sc.items) {
            QJsonObject io{{QStringLiteral("id"), i.id}, {QStringLiteral("source"), i.source},
                           {QStringLiteral("transform"), ::toJson(i.transform)},
                           {QStringLiteral("visible"), i.visible}, {QStringLiteral("locked"), i.locked}};
            if (!i.showTransition.isEmpty()) io.insert(QStringLiteral("show"), QJsonObject{{QStringLiteral("transition"), i.showTransition}, {QStringLiteral("ms"), i.showMs}});
            if (!i.hideTransition.isEmpty()) io.insert(QStringLiteral("hide"), QJsonObject{{QStringLiteral("transition"), i.hideTransition}, {QStringLiteral("ms"), i.hideMs}});
            items.append(io);
        }
        QJsonObject so{{QStringLiteral("id"), sc.id}, {QStringLiteral("name"), sc.name},
                       {QStringLiteral("items"), items}};
        if (!sc.transition.isEmpty()) so.insert(QStringLiteral("transition"), sc.transition);
        if (sc.transitionMs > 0) so.insert(QStringLiteral("transitionMs"), sc.transitionMs);
        if (sc.group) {
            so.insert(QStringLiteral("group"), true);
            so.insert(QStringLiteral("width"), sc.width);
            so.insert(QStringLiteral("height"), sc.height);
        }
        scns.append(so);
    }
    QJsonArray made, quick;
    for (const TransitionDef &t : transitions)
        made.append(QJsonObject{{QStringLiteral("id"), t.id}, {QStringLiteral("name"), t.name},
                                {QStringLiteral("settings"), QJsonObject::fromVariantMap(t.settings)}});
    for (const QuickTransition &q : quickTransitions)
        quick.append(QJsonObject{{QStringLiteral("transition"), q.transition}, {QStringLiteral("durationMs"), q.durationMs}});
    QJsonObject out{{QStringLiteral("version"), 1},
                    {QStringLiteral("name"), name},
                    {QStringLiteral("sources"), srcs},
                    {QStringLiteral("scenes"), scns},
                    {QStringLiteral("currentScene"), currentScene},
                    {QStringLiteral("nextId"), nextId},
                    {QStringLiteral("transitions"), made},
                    {QStringLiteral("currentTransition"), currentTransition},
                    {QStringLiteral("transitionMs"), transitionMs},
                    {QStringLiteral("quickTransitions"), quick},
                    {QStringLiteral("hotkeys"), QJsonObject::fromVariantMap(hotkeys)},
                    {QStringLiteral("switcher"), QJsonObject::fromVariantMap(switcher)}};
    if (canvasWidth > 0 && canvasHeight > 0) {
        out.insert(QStringLiteral("canvasWidth"), canvasWidth);
        out.insert(QStringLiteral("canvasHeight"), canvasHeight);
    }
    return out;
}

const TransitionDef *Collection::transition(const QString &id) const {
    for (const TransitionDef &t : transitions)
        if (t.id == id) return &t;
    return nullptr;
}

Collection Collection::fromJson(const QJsonObject &json) {
    Collection c;
    c.name = json.value(QStringLiteral("name")).toString(c.name);
    // Sizes no canvas can be are as good as none: rescaling from them would wreck it.
    const int keptWidth = json.value(QStringLiteral("canvasWidth")).toInt(), keptHeight = json.value(QStringLiteral("canvasHeight")).toInt();
    if (keptWidth >= 16 && keptWidth <= 8192 && keptHeight >= 16 && keptHeight <= 8192) {
        c.canvasWidth = keptWidth;
        c.canvasHeight = keptHeight;
    }
    for (const QJsonValue &v : json.value(QStringLiteral("sources")).toArray()) {
        const QJsonObject o = v.toObject();
        c.sources.append({o.value(QStringLiteral("id")).toString(), o.value(QStringLiteral("kind")).toString(),
                          o.value(QStringLiteral("name")).toString(),
                          o.value(QStringLiteral("settings")).toObject().toVariantMap()});
    }
    for (const QJsonValue &v : json.value(QStringLiteral("scenes")).toArray()) {
        const QJsonObject o = v.toObject();
        Scene sc{o.value(QStringLiteral("id")).toString(), o.value(QStringLiteral("name")).toString(), {},
                 o.value(QStringLiteral("group")).toBool(), o.value(QStringLiteral("width")).toInt(),
                 o.value(QStringLiteral("height")).toInt()};
        for (const QJsonValue &iv : o.value(QStringLiteral("items")).toArray()) {
            const QJsonObject io = iv.toObject();
            SceneItem it;
            it.id = io.value(QStringLiteral("id")).toString();
            it.source = io.value(QStringLiteral("source")).toString();
            it.transform = transformFrom(io.value(QStringLiteral("transform")).toObject());
            it.visible = io.value(QStringLiteral("visible")).toBool(true);
            it.locked = io.value(QStringLiteral("locked")).toBool();
            for (const bool show : {true, false}) {
                const QJsonObject t = io.value(show ? QStringLiteral("show") : QStringLiteral("hide")).toObject();
                (show ? it.showTransition : it.hideTransition) = t.value(QStringLiteral("transition")).toString();
                (show ? it.showMs : it.hideMs) = std::clamp(t.value(QStringLiteral("ms")).toInt(300), 0, 20000);
            }
            // An item whose source is gone would draw nothing and could never be fixed.
            if (c.source(it.source)) sc.items.append(it);
        }
        sc.transition = o.value(QStringLiteral("transition")).toString();
        sc.transitionMs = std::clamp(o.value(QStringLiteral("transitionMs")).toInt(), 0, 20000);
        c.scenes.append(sc);
    }
    for (const QJsonValue &v : json.value(QStringLiteral("transitions")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString id = o.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || id == QLatin1String("cut") || id == QLatin1String("fade") || c.transition(id)) continue;
        c.transitions.append({id, o.value(QStringLiteral("name")).toString(),
                              o.value(QStringLiteral("settings")).toObject().toVariantMap()});
    }
    auto known = [&c](const QString &id) { return id == QLatin1String("cut") || id == QLatin1String("fade") || c.transition(id); };
    c.currentTransition = json.value(QStringLiteral("currentTransition")).toString(c.currentTransition);
    if (!known(c.currentTransition)) c.currentTransition = QStringLiteral("fade");
    c.transitionMs = std::clamp(json.value(QStringLiteral("transitionMs")).toInt(c.transitionMs), 0, 20000);
    if (json.contains(QStringLiteral("quickTransitions"))) {
        c.quickTransitions.clear();
        for (const QJsonValue &v : json.value(QStringLiteral("quickTransitions")).toArray()) {
            const QJsonObject o = v.toObject();
            const QString id = o.value(QStringLiteral("transition")).toString();
            if (known(id)) c.quickTransitions.append({id, std::clamp(o.value(QStringLiteral("durationMs")).toInt(300), 0, 20000)});
        }
    }
    for (Scene &sc : c.scenes)
        if (!sc.transition.isEmpty() && !known(sc.transition)) sc.transition.clear();
    c.hotkeys = json.value(QStringLiteral("hotkeys")).toObject().toVariantMap();
    c.switcher = json.value(QStringLiteral("switcher")).toObject().toVariantMap();
    c.currentScene = json.value(QStringLiteral("currentScene")).toString();
    c.nextId = std::max(1, json.value(QStringLiteral("nextId")).toInt(1));
    c.repair();
    if (!c.current() || c.current()->group) {
        c.currentScene.clear();
        for (const Scene &s : c.scenes)
            if (!s.group) {
                c.currentScene = s.id;
                break;
            }
    }
    return c;
}

const SceneItem *Collection::findItem(const QString &itemId, const Scene **owner) const {
    for (const Scene &s : scenes)
        if (const SceneItem *i = s.item(itemId)) {
            if (owner) *owner = &s;
            return i;
        }
    return nullptr;
}

SceneItem *Collection::findItem(const QString &itemId, Scene **owner) {
    for (Scene &s : scenes)
        if (SceneItem *i = s.item(itemId)) {
            if (owner) *owner = &s;
            return i;
        }
    return nullptr;
}

void Collection::repair() {
    // Each id once (the first kept), and none empty.
    QSet<QString> seen;
    const auto fresh = [&seen](const QString &id) { return !id.isEmpty() && !seen.contains(id) && (seen.insert(id), true); };
    sources.removeIf([&](const SourceDef &src) { return !fresh(src.id); });
    scenes.removeIf([&](const Scene &sc) { return !fresh(sc.id); });
    for (Scene &sc : scenes) {
        sc.items.removeIf([&](const SceneItem &it) { return !fresh(it.id); });
        // A group is drawn at its own size: never none, and no bigger than a picture can be.
        if (sc.group) { sc.width = std::clamp(sc.width, 1, kMaxGroupSize); sc.height = std::clamp(sc.height, 1, kMaxGroupSize); }
    }
    // A studio that shows itself, however deep, would be drawn for ever.
    for (Scene &sc : scenes)
        sc.items.removeIf([&](const SceneItem &it) {
            const SourceDef *src = source(it.source);
            const QString shown = src ? shownStudio(*src) : QString();
            return !shown.isEmpty() && shows(shown, sc.id);
        });
    // The next id past every one in use, whatever the file said. One at the
    // very top of the range is left alone: the count must have room to go on.
    const auto past = [this](const QString &id) {
        bool ok = false;
        const int n = id.mid(1).toInt(&ok);
        if (ok && n >= nextId && n < std::numeric_limits<int>::max() - 1) nextId = n + 1;
    };
    for (const QString &id : std::as_const(seen)) past(id);
    for (const TransitionDef &t : std::as_const(transitions)) past(t.id);
}

QString newPassphrase() {
    // No 0, O, 1, l or capitals: 5 bits a character, 80 in all.
    static constexpr char kLetters[] = "abcdefghijkmnpqrstuvwxyz23456789";
    QString out;
    for (int i = 0; i < 16; ++i) out += QLatin1Char(kLetters[QRandomGenerator::system()->bounded(int(sizeof kLetters - 1))]);
    return out;
}

bool passphraseFits(const QString &passphrase) {
    if (passphrase.size() < kPassphraseMin || passphrase.size() > kPassphraseMax) return false;
    return std::all_of(passphrase.cbegin(), passphrase.cend(), [](QChar c) {
        return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
               (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || QStringLiteral("-_.~").contains(c);
    });
}

bool Collection::protectNetworkStreams() {
    bool changed = false;
    const QString key = QLatin1String(kPassphraseKey);
    for (SourceDef &s : sources) {
        if (!isNetworkStream(s)) continue;
        const QVariant now = s.settings.value(key);
        if (now.isValid() && now.userType() == QMetaType::QString && (now.toString().isEmpty() || passphraseFits(now.toString()))) continue;
        s.settings.insert(key, newPassphrase());
        changed = true;
    }
    return changed;
}

Collection Collection::withoutPassphrases() const {
    Collection out = *this;
    for (SourceDef &s : out.sources) s.settings.remove(QLatin1String(kPassphraseKey));
    return out;
}

bool Collection::holdsPassphrase() const {
    return std::any_of(sources.cbegin(), sources.cend(), [](const SourceDef &s) { return !s.settings.value(QLatin1String(kPassphraseKey)).toString().isEmpty(); });
}

QString Collection::shownStudio(const SourceDef &source) const {
    if (source.kind != QLatin1String("studio") && source.kind != QLatin1String("group")) return {};
    return source.settings.value(QStringLiteral("studio")).toString();
}

bool Collection::shows(const QString &outer, const QString &inner) const {
    // Depth-first through studio and group sources, each studio once.
    QStringList seen, todo{outer};
    while (!todo.isEmpty()) {
        const QString id = todo.takeLast();
        if (id == inner) return true;
        if (seen.contains(id)) continue;
        seen.append(id);
        if (const Scene *s = scene(id))
            for (const SceneItem &i : s->items)
                if (const SourceDef *src = source(i.source); src && !shownStudio(*src).isEmpty())
                    todo.append(shownStudio(*src));
    }
    return false;
}

void Collection::prune() {
    for (bool changed = true; changed;) {
        changed = false;
        // Studio and group sources whose studio is gone, with their items.
        for (int i = sources.size() - 1; i >= 0; --i) {
            const QString shown = shownStudio(sources[i]);
            if (shown.isEmpty() || scene(shown)) continue;
            const QString id = sources[i].id;
            for (Scene &s : scenes) s.items.removeIf([&](const SceneItem &it) { return it.source == id; });
            sources.removeAt(i);
            changed = true;
        }
        // Sources no item shows.
        for (int i = sources.size() - 1; i >= 0; --i) {
            bool used = false;
            for (const Scene &s : scenes)
                for (const SceneItem &it : s.items) used |= it.source == sources[i].id;
            if (!used) {
                sources.removeAt(i);
                changed = true;
            }
        }
        // Groups no group source shows.
        for (int i = scenes.size() - 1; i >= 0; --i) {
            if (!scenes[i].group) continue;
            bool used = false;
            for (const SourceDef &src : sources) used |= src.kind == QLatin1String("group") && shownStudio(src) == scenes[i].id;
            if (!used) {
                scenes.removeAt(i);
                changed = true;
            }
        }
    }
}

QPointF alignmentPoint(Transform::Alignment alignment) {
    const int index = std::clamp(int(alignment), 0, 8);
    return {double(index % 3) / 2, double(index / 3) / 2};
}

Outline itemOutline(const Transform &t, int w, int h) {
    double cw, ch;
    croppedSize(t, w, h, &cw, &ch);
    Outline o{t.x, t.y, cw * t.scaleX, ch * t.scaleY, t.rotation};
    if (t.bounds != Transform::Bounds::None) {
        o.width = t.boundsWidth;
        o.height = t.boundsHeight;
    }
    // (x, y) is the anchor before rotation; the box turns about the pivot, so
    // its unrotated top-left moves by the pivot's offset from the centre.
    const QPointF a = alignmentPoint(t.positionAlignment), p = alignmentPoint(t.rotationAlignment);
    const double r = t.rotation * M_PI / 180.0, c = std::cos(r), s = std::sin(r);
    const double dx = (0.5 - p.x()) * o.width, dy = (0.5 - p.y()) * o.height;
    o.x += -a.x() * o.width + (c - 1) * dx - s * dy;
    o.y += -a.y() * o.height + s * dx + (c - 1) * dy;
    return o;
}

QPointF itemPivot(const Transform &t, int w, int h) {
    const Outline o = itemOutline(t, w, h);
    const QPointF a = alignmentPoint(t.positionAlignment), p = alignmentPoint(t.rotationAlignment);
    return {t.x + (p.x() - a.x()) * o.width, t.y + (p.y() - a.y()) * o.height};
}

engine::Layer itemLayer(const Transform &t, int w, int h) {
    double cw, ch;
    croppedSize(t, w, h, &cw, &ch);
    const Outline o = itemOutline(t, w, h);
    const QPointF a = alignmentPoint(t.boundsAlignment);
    // The content in box-local coordinates, and the cropped part of the
    // source in 0..1, mirrored before any clipping.
    double lx = 0, ly = 0, lw = o.width, lh = o.height;
    const Crop crop = cropWithin(t, w, h);
    double u0 = double(crop.left) / w, u1 = double(w - crop.right) / w;
    double v0 = double(crop.top) / h, v1 = double(h - crop.bottom) / h;
    if (t.flipH) std::swap(u0, u1);
    if (t.flipV) std::swap(v0, v1);
    if (t.bounds != Transform::Bounds::None && t.bounds != Transform::Bounds::Stretch) {
        double scale = 1;
        switch (t.bounds) {
        case Transform::Bounds::Fit: scale = std::min(o.width / cw, o.height / ch); break;
        case Transform::Bounds::Fill:
        case Transform::Bounds::Cover: scale = std::max(o.width / cw, o.height / ch); break;
        case Transform::Bounds::Width: scale = o.width / cw; break;
        case Transform::Bounds::Height: scale = o.height / ch; break;
        case Transform::Bounds::MaxOnly: scale = std::min({1.0, o.width / cw, o.height / ch}); break;
        default: break;
        }
        lw = cw * scale;
        lh = ch * scale;
        lx = (o.width - lw) * a.x();
        ly = (o.height - lh) * a.y();
        // Fill always clips to the box; the others only when asked.
        if ((t.clipToBounds || t.bounds == Transform::Bounds::Fill) && lw > 0 && lh > 0) {
            const double left = std::max(0.0, lx), top = std::max(0.0, ly);
            const double right = std::min(o.width, lx + lw), bottom = std::min(o.height, ly + lh);
            const double du = u1 - u0, dv = v1 - v0;
            u1 = u0 + du * (right - lx) / lw;
            u0 += du * (left - lx) / lw;
            v1 = v0 + dv * (bottom - ly) / lh;
            v0 += dv * (top - ly) / lh;
            lx = left;
            ly = top;
            lw = right - left;
            lh = bottom - top;
        }
    }
    // The engine rotates each layer about its own centre, so rotate the
    // content's centre about the outline's.
    const double r = t.rotation * M_PI / 180.0, c = std::cos(r), s = std::sin(r);
    const double dx = lx + (lw - o.width) / 2, dy = ly + (lh - o.height) / 2;
    engine::Layer l;
    l.x = float(o.x + o.width / 2 + c * dx - s * dy - lw / 2);
    l.y = float(o.y + o.height / 2 + s * dx + c * dy - lh / 2);
    l.w = float(lw);
    l.h = float(lh);
    l.rotation = float(r);
    l.u0 = float(u0);
    l.v0 = float(v0);
    l.u1 = float(u1);
    l.v1 = float(v1);
    l.filter = t.scale;
    l.opacity = float(t.opacity);
    l.blend = t.blend;
    l.blendSpace = t.blendSpace;
    return l;
}

QTransform itemMapping(const Transform &t, int w, int h) {
    const auto l = itemLayer(t, w, h);
    const double du = (l.u1 - l.u0) * w, dv = (l.v1 - l.v0) * h;
    if (du == 0 || dv == 0) return {};
    const double sx = l.w / du, sy = l.h / dv;
    const double r = t.rotation * M_PI / 180., c = std::cos(r), s = std::sin(r);
    const double cx = (l.u0 + l.u1) * w / 2., cy = (l.v0 + l.v1) * h / 2.;
    return {c * sx, s * sx, -s * sy, c * sy,
            l.x + l.w / 2. - c * sx * cx + s * sy * cy,
            l.y + l.h / 2. - s * sx * cx - c * sy * cy};
}

Outline mappedOutline(const QTransform &mapping, const Outline &o) {
    const QPointF centre(o.x + o.width / 2, o.y + o.height / 2);
    const double r = o.rotation * M_PI / 180.;
    const auto origin = mapping.map(centre);
    const auto x = mapping.map(centre + QPointF(std::cos(r), std::sin(r))) - origin;
    const auto y = mapping.map(centre + QPointF(-std::sin(r), std::cos(r))) - origin;
    const double w = o.width * std::hypot(x.x(), x.y()), h = o.height * std::hypot(y.x(), y.y());
    return {origin.x() - w / 2, origin.y() - h / 2, w, h, std::atan2(x.y(), x.x()) * 180. / M_PI};
}

void setItemBox(Transform &t, int w, int h, double x, double y, double width, double height) {
    // The inverse of itemOutline for this size.
    const QPointF a = alignmentPoint(t.positionAlignment), p = alignmentPoint(t.rotationAlignment);
    const double r = t.rotation * M_PI / 180.0, c = std::cos(r), s = std::sin(r);
    const double dx = (0.5 - p.x()) * width, dy = (0.5 - p.y()) * height;
    t.x = x + a.x() * width - (c - 1) * dx + s * dy;
    t.y = y + a.y() * height - s * dx - (c - 1) * dy;
    if (t.bounds != Transform::Bounds::None) {
        t.boundsWidth = width;
        t.boundsHeight = height;
        return;
    }
    double cw, ch;
    croppedSize(t, w, h, &cw, &ch);
    t.scaleX = width / cw;
    t.scaleY = height / ch;
}
