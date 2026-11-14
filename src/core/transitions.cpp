#include "transitions.h"

#include <QColor>
#include <QCoreApplication>

#include <algorithm>

namespace Transitions {
namespace {

QString tr(const char *text) { return QCoreApplication::translate("Transitions", text); }

const char *const kKinds[] = {"cut", "fade", "colour", "swipe", "cover", "uncover", "slide", "wipe", "stinger"};
// A stinger's studios change with a cut; the video goes over it.
const engine::Transition kTypes[] = {engine::Transition::Cut, engine::Transition::Fade, engine::Transition::FadeColour,
                                     engine::Transition::Swipe, engine::Transition::Cover, engine::Transition::Uncover,
                                     engine::Transition::Slide, engine::Transition::Wipe, engine::Transition::Cut};
const char *const kSides[] = {"left", "right", "top", "bottom"};
// In engine::WipeMask order.
const char *const kMasks[] = {"left", "right", "top", "bottom", "topLeft", "topRight", "bottomLeft", "bottomRight",
                              "barnVertical", "barnHorizontal", "barnDiagonal", "iris", "box", "diamond", "clock", "fan",
                              "blindsHorizontal", "blindsVertical", "stripes", "checkerboard", "squares", "circles", "spiral",
                              "zigzag", "sinus", "curtain", "burst", "cloud", "fractal"};
static_assert(std::size(kMasks) == size_t(engine::WipeMask::Count));

template <size_t N> int indexOf(const char *const (&names)[N], const QString &name, int fallback) {
    for (size_t i = 0; i < N; ++i)
        if (name == QLatin1String(names[i])) return int(i);
    return fallback;
}

}  // namespace

QVariantList kinds() {
    const QString names[] = {tr("Cut"), tr("Fade"), tr("Fade through colour"), tr("Swipe"), tr("Cover"),
                             tr("Uncover"), tr("Slide"), tr("Wipe"), tr("Stinger")};
    QVariantList out;
    for (size_t i = 0; i < std::size(kKinds); ++i) {
        const QString id = kKinds[i];
        out.append(QVariantMap{{"id", id}, {"name", names[i]},
                               {"side", id == "swipe" || id == "cover" || id == "uncover" || id == "slide"},
                               {"colour", id == "colour"}, {"mask", id == "wipe"}, {"stinger", id == "stinger"},
                               {"timed", id != "cut" && id != "stinger"}});
    }
    return out;
}

QVariantList sides() {
    const QString names[] = {tr("From the left"), tr("From the right"), tr("From the top"), tr("From the bottom")};
    QVariantList out;
    for (size_t i = 0; i < std::size(kSides); ++i) out.append(QVariantMap{{"id", QString(kSides[i])}, {"name", names[i]}});
    return out;
}

QVariantList masks() {
    const QString names[] = {
        tr("Linear from the left"), tr("Linear from the right"), tr("Linear from the top"), tr("Linear from the bottom"),
        tr("Diagonal from top left"), tr("Diagonal from top right"), tr("Diagonal from bottom left"), tr("Diagonal from bottom right"),
        tr("Barn doors, vertical"), tr("Barn doors, horizontal"), tr("Barn doors, diagonal"), tr("Iris"), tr("Box"),
        tr("Diamond"), tr("Clock"), tr("Fan"), tr("Blinds, horizontal"), tr("Blinds, vertical"), tr("Stripes"),
        tr("Checkerboard"), tr("Squares"), tr("Circles"), tr("Spiral"), tr("Zig-zag"), tr("Sine wave"), tr("Curtain"),
        tr("Burst"), tr("Cloud"), tr("Fractal")};
    QVariantList out;
    for (size_t i = 0; i < std::size(kMasks); ++i) out.append(QVariantMap{{"id", QString(kMasks[i])}, {"name", names[i]}});
    return out;
}

QVariantMap normalized(const QVariantMap &s) {
    QVariantMap out = s;
    out["kind"] = QString(kKinds[indexOf(kKinds, s.value("kind").toString(), 1)]);
    out["side"] = QString(kSides[indexOf(kSides, s.value("side").toString(), 0)]);
    const QColor colour(s.value("colour", QStringLiteral("#000000")).toString());
    out["colour"] = colour.isValid() ? colour.name() : QStringLiteral("#000000");
    out["point"] = std::clamp(s.value("point", 0.5).toDouble(), 0.0, 1.0);
    out["mask"] = QString(kMasks[indexOf(kMasks, s.value("mask").toString(), 0)]);
    out["softness"] = std::clamp(s.value("softness", 0.1).toDouble(), 0.0, 1.0);
    out["invert"] = s.value("invert", false).toBool();
    out["file"] = s.value("file").toString();
    out["cutMs"] = std::clamp(s.value("cutMs", 500).toInt(), 0, 60000);
    out["cutFrames"] = std::clamp(s.value("cutFrames", 15).toInt(), 0, 100000);
    out["cutUnit"] = s.value("cutUnit").toString() == QLatin1String("frames") ? QStringLiteral("frames") : QStringLiteral("ms");
    out["audioFade"] = s.value("audioFade").toString() == QLatin1String("outIn") ? QStringLiteral("outIn") : QStringLiteral("crossfade");
    const QString monitor = s.value("monitor").toString();
    out["monitor"] = monitor == QLatin1String("only") || monitor == QLatin1String("both") ? monitor : QStringLiteral("off");
    static const QStringList mattes{QStringLiteral("none"), QStringLiteral("horizontal"), QStringLiteral("vertical"), QStringLiteral("mask"),
                                    QStringLiteral("separate")};
    out["matte"] = mattes.contains(s.value("matte").toString()) ? s.value("matte").toString() : QStringLiteral("none");
    out["matteInvert"] = s.value("matteInvert", false).toBool();
    out["matteFile"] = s.value("matteFile").toString();
    return out;
}

engine::TransitionStyle style(const QVariantMap &settings) {
    const QVariantMap s = normalized(settings);
    engine::TransitionStyle style;
    style.type = kTypes[indexOf(kKinds, s.value("kind").toString(), 1)];
    style.side = engine::Side(indexOf(kSides, s.value("side").toString(), 0));
    const QColor colour(s.value("colour").toString());
    style.colour = {engine::srgbToLinear(float(colour.redF())), engine::srgbToLinear(float(colour.greenF())),
                    engine::srgbToLinear(float(colour.blueF())), 1.f};
    style.colourPoint = float(s.value("point").toDouble());
    style.mask = engine::WipeMask(indexOf(kMasks, s.value("mask").toString(), 0));
    style.softness = float(s.value("softness").toDouble());
    style.invert = s.value("invert").toBool();
    return style;
}

bool isStinger(const QVariantMap &settings) { return settings.value("kind").toString() == QLatin1String("stinger"); }

QVariantList all(const Collection &c) {
    QVariantList out{QVariantMap{{"id", "cut"}, {"name", tr("Cut")}, {"settings", normalized({{"kind", "cut"}})}, {"builtIn", true}},
                     QVariantMap{{"id", "fade"}, {"name", tr("Fade")}, {"settings", normalized({{"kind", "fade"}})}, {"builtIn", true}}};
    for (const TransitionDef &t : c.transitions)
        out.append(QVariantMap{{"id", t.id}, {"name", t.name}, {"settings", normalized(t.settings)}, {"builtIn", false}});
    return out;
}

QVariantMap settings(const Collection &c, const QString &id) {
    if (id == QLatin1String("cut") || id == QLatin1String("fade")) return normalized({{"kind", id}});
    const TransitionDef *t = c.transition(id);
    return t ? normalized(t->settings) : QVariantMap();
}

bool exists(const Collection &c, const QString &id) {
    return id == QLatin1String("cut") || id == QLatin1String("fade") || c.transition(id);
}

Choice choiceFor(const Collection &c, const QString &scene) {
    Choice choice{c.currentTransition, c.transitionMs};
    if (const Scene *s = c.scene(scene)) {
        if (!s->transition.isEmpty() && exists(c, s->transition)) choice.transition = s->transition;
        if (s->transitionMs > 0) choice.durationMs = s->transitionMs;
    }
    if (settings(c, choice.transition).value("kind").toString() == QLatin1String("cut")) choice.durationMs = 0;
    return choice;
}

}  // namespace Transitions
