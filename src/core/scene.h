#pragma once

#include <QJsonObject>
#include <QPointF>
#include <QTransform>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include "engine/frame.h"

// The production's content: what can be shown (sources), how it is arranged
// (scenes of items), which scene is live. Plain values, JSON both ways; the
// Production object owns one and keeps the undo history.

// Something that makes pictures. Kinds: "display" and "window" (screen
// capture; settings: cursor, restoreToken), "colour" (colour, width, height),
// "image" (path), "text" (text, font, size, colour, background, …),
// "browser" (url, or path when local; width, height, fps, css,
// shutdownHidden, refreshOnActive).
struct SourceDef {
    QString id;
    QString kind;
    QString name;
    QVariantMap settings;

    static bool isAudioKind(const QString &kind) {
        return kind == QLatin1String("audio-input") || kind == QLatin1String("audio-output") ||
               kind == QLatin1String("audio-application");
    }
    bool hasAudio() const {
        return isAudioKind(kind) || kind == QLatin1String("media") || kind == QLatin1String("device") || kind == QLatin1String("browser");
    }
    bool operator==(const SourceDef &o) const = default;
};

// How an item sits on the canvas. The outline is the bounds box when there
// is one, otherwise the cropped source at its scale; it is placed with its
// chosen position anchor at (x, y) before rotation. The rotation pivot is
// independent; defaults preserve top-left placement and centre rotation.
struct Transform {
    enum class Bounds { None, Stretch, Fit, Fill, Cover, Width, Height, MaxOnly };
    enum class Alignment { TopLeft, Top, TopRight, Left, Centre, Right, BottomLeft, Bottom, BottomRight };

    double x = 0, y = 0;
    double scaleX = 1, scaleY = 1;   // used when bounds is None
    double rotation = 0;             // degrees, clockwise
    int cropLeft = 0, cropTop = 0, cropRight = 0, cropBottom = 0;  // source pixels
    bool flipH = false, flipV = false;
    Bounds bounds = Bounds::None;
    double boundsWidth = 0, boundsHeight = 0;
    Alignment positionAlignment = Alignment::TopLeft;
    Alignment rotationAlignment = Alignment::Centre;
    Alignment boundsAlignment = Alignment::Centre;
    bool clipToBounds = false;
    engine::ScaleFilter scale = engine::ScaleFilter::Auto;  // how it is resampled
    double opacity = 1;
    engine::BlendMode blend = engine::BlendMode::Normal;
    engine::BlendSpace blendSpace = engine::BlendSpace::Linear;

    bool operator==(const Transform &o) const = default;
};

struct SceneItem {
    QString id;
    QString source;       // SourceDef id
    Transform transform;
    bool visible = true;
    bool locked = false;
    // Shown and hidden through a transition (an id, as for studios; empty
    // for at once) over so many milliseconds.
    QString showTransition{}, hideTransition{};
    int showMs = 300, hideMs = 300;

    bool operator==(const SceneItem &o) const = default;
};

// As big as a group's picture can be, as a canvas or a filter's.
constexpr int kMaxGroupSize = 16384;

// A studio (what the interface calls a scene) — or, with `group` set, the
// private contents of a group: a box of width x height that one group source
// shows, never listed among the studios.
struct Scene {
    QString id;
    QString name;
    QVector<SceneItem> items;  // bottom to top
    bool group = false;
    int width = 0, height = 0; // a group's box; 0 for a studio (the canvas)
    // Going live with this studio uses this transition and duration instead
    // of the collection's; empty and 0 follow the collection.
    QString transition{};
    int transitionMs = 0;

    const SceneItem *item(const QString &itemId) const;
    SceneItem *item(const QString &itemId);
    bool operator==(const Scene &o) const = default;
};

// A transition people made and named; "cut" and "fade" are built in and
// never stored. Settings: see core/transitions.h.
struct TransitionDef {
    QString id;
    QString name;
    QVariantMap settings;
    bool operator==(const TransitionDef &o) const = default;
};
// A one-click transition button.
struct QuickTransition {
    QString transition;
    int durationMs = 300;
    bool operator==(const QuickTransition &o) const = default;
};

struct Collection {
    QString name = QStringLiteral("Default");
    QVector<SourceDef> sources;
    QVector<Scene> scenes;
    QString currentScene;
    int nextId = 1;        // ids are short and deterministic: s1, i2, c3 …
    QVector<TransitionDef> transitions;
    QString currentTransition = QStringLiteral("fade");
    int transitionMs = 300;
    QVector<QuickTransition> quickTransitions{{QStringLiteral("cut"), 0}, {QStringLiteral("fade"), 300}};
    // Keys for actions on this collection's studios, sources, transitions
    // and filters: "action argument" → key ("Ctrl+F1").
    QVariantMap hotkeys;
    // The automatic studio switcher: {enabled, rules: [{field: class|title,
    // pattern (a regular expression), studio}], otherwise: studio or ""}.
    QVariantMap switcher;
    // The canvas the layout here is at: positions and sizes are in its pixels, and
    // a profile or a display can bring another. 0: not known (an older file).
    int canvasWidth = 0, canvasHeight = 0;
    const TransitionDef *transition(const QString &id) const;

    QString makeId(QChar prefix) { return QString(prefix) + QString::number(nextId++); }
    const SourceDef *source(const QString &id) const;
    SourceDef *source(const QString &id);
    const Scene *scene(const QString &id) const;
    Scene *scene(const QString &id);
    const Scene *current() const { return scene(currentScene); }
    Scene *current() { return scene(currentScene); }
    // An item wherever it is (ids are unique across the collection), and the
    // studio or group it belongs to.
    const SceneItem *findItem(const QString &itemId, const Scene **owner = nullptr) const;
    SceneItem *findItem(const QString &itemId, Scene **owner = nullptr);

    QJsonObject toJson() const;
    static Collection fromJson(const QJsonObject &json);
    bool operator==(const Collection &o) const = default;

    // The studio (or group) a "studio"/"group" source shows; empty otherwise.
    QString shownStudio(const SourceDef &source) const;
    // Whether studio `outer` shows studio `inner`, itself or through others.
    bool shows(const QString &outer, const QString &inner) const;
    // Made safe to use after reading a file: ids once each, no studio inside
    // itself, groups with a size, the next id past every one taken.
    void repair();
    // Drops what nothing uses any more: groups no group source shows, studio
    // and group sources whose studio is gone (with their items), sources no
    // item shows.
    void prune();
    // Every network stream with a passphrase that will do. One with none yet, or
    // one no listener could use, gets a new one; one left empty on purpose stays
    // off. True when any changed.
    bool protectNetworkStreams();
    // As it goes to someone else: with no passphrases, so that opening it makes new ones.
    Collection withoutPassphrases() const;
    bool holdsPassphrase() const;
};

// A network stream's passphrase, in the setting of this name. Empty is off, and
// nothing in the setting's absence: the listener's address never holds it.
inline constexpr int kPassphraseMin = 10, kPassphraseMax = 79;  // what SRT takes
inline constexpr const char *kPassphraseKey = "passphrase";
inline constexpr const char *kRtmpStreamName = "omabroadcast";
inline bool isNetworkStream(const SourceDef &s) { return s.kind == QLatin1String("media") && s.settings.contains(QStringLiteral("port")); }
// Random, from the system's secure source, in letters and digits nobody mistakes
// on a phone's keyboard.
QString newPassphrase();
// The right length, in what goes unchanged into an address on either protocol.
bool passphraseFits(const QString &passphrase);

// Geometry, shared by the compositor and the preview's handles.
struct Outline {
    double x = 0, y = 0, width = 0, height = 0, rotation = 0;  // rotation in degrees
};
QPointF alignmentPoint(Transform::Alignment alignment);
QPointF itemPivot(const Transform &t, int sourceWidth, int sourceHeight);
// The item's outline on the canvas for a source of this pixel size.
Outline itemOutline(const Transform &t, int sourceWidth, int sourceHeight);
// The engine layer that draws it (source id left for the caller).
engine::Layer itemLayer(const Transform &t, int sourceWidth, int sourceHeight);
// Source pixel coordinates through the drawn content (crop, bounds and flips).
QTransform itemMapping(const Transform &t, int sourceWidth, int sourceHeight);
// Maps the centre and axis lengths. A nonuniformly scaled rotated rectangle
// can shear; Outline describes its axes but cannot express that shear.
Outline mappedOutline(const QTransform &mapping, const Outline &outline);
// Changes the transform so the outline becomes this box, keeping rotation:
// a bounded item changes its bounds, an unbounded one its scale.
void setItemBox(Transform &t, int sourceWidth, int sourceHeight, double x, double y, double width, double height);
