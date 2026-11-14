#include "video_filter_settings.h"
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QRegularExpression>
#include <QTextStream>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>
#include <string_view>

namespace {
using T = engine::VideoFilterType;
enum class Kind { Number, Choice, Colour, File, Toggle };
struct Option { const char *value, *label; };
struct Param {
    const char *key, *label, *unit;
    Kind kind;
    double initial = 0, minimum = 0, maximum = 0, step = 1;
    const char *text = "";           // initial colour, choice or file
    std::vector<Option> options = {};
    const char *showKey = nullptr;   // shown only while that parameter has showValue
    const char *showValue = nullptr;
    const char *tip = "";
    const char *wheel = nullptr;     // part of that colour wheel: its "x", "y" or "level"
    const char *axis = nullptr;
};
struct Preset { const char *label; int width, height; };
struct Def {
    T type;
    const char *key, *label, *description;
    std::vector<Param> params;
    bool async = false;
    std::vector<Preset> presets = {};
};
Param number(const char *key, const char *label, const char *unit, double initial, double minimum, double maximum, double step, const char *tip = "") {
    return {key, label, unit, Kind::Number, initial, minimum, maximum, step, "", {}, nullptr, nullptr, tip};
}
Param colour(const char *key, const char *label, const char *initial) { return {key, label, "", Kind::Colour, 0, 0, 0, 1, initial}; }
Param choice(const char *key, const char *label, const char *initial, std::vector<Option> options, const char *tip = "") {
    return {key, label, "", Kind::Choice, 0, 0, 0, 1, initial, std::move(options), nullptr, nullptr, tip};
}
Param toggle(const char *key, const char *label, bool initial, const char *tip = "") {
    return {key, label, "", Kind::Toggle, initial ? 1.0 : 0.0, 0, 1, 1, "", {}, nullptr, nullptr, tip};
}
Param file(const char *key, const char *label) { return {key, label, "", Kind::File}; }
Param when(Param p, const char *key, const char *value) { p.showKey = key; p.showValue = value; return p; }
// One colour wheel: where its point is (x, y inside the unit circle) and its level.
void wheel(std::vector<Param> &params, const char *key, const char *label, double initial, double minimum, double maximum, const char *tip) {
    static const char *const keys[][2] = {{"liftX", "liftY"}, {"gammaX", "gammaY"}, {"gainX", "gainY"}, {"offsetX", "offsetY"}};
    const int i = std::string_view(key) == "lift" ? 0 : std::string_view(key) == "gamma" ? 1 : std::string_view(key) == "gain" ? 2 : 3;
    for (int a = 0; a < 2; ++a) {
        Param p = number(keys[i][a], label, "", 0, -1, 1, 0.01);
        p.wheel = key; p.axis = a ? "y" : "x";
        params.push_back(p);
    }
    Param level = number(key, label, "", initial, minimum, maximum, 0.01, tip);
    level.wheel = key; level.axis = "level";
    params.push_back(level);
}
std::vector<Param> wheelParams() {
    std::vector<Param> params;
    wheel(params, "lift", QT_TRANSLATE_NOOP("VideoFilters", "Lift"), 0, -1, 1, QT_TRANSLATE_NOOP("VideoFilters", "The shadows: the wheel pushes them towards a colour, the level raises or lowers black"));
    wheel(params, "gamma", QT_TRANSLATE_NOOP("VideoFilters", "Gamma"), 0, -1, 1, QT_TRANSLATE_NOOP("VideoFilters", "The mid-tones: the wheel pushes them towards a colour, the level brightens or darkens them"));
    wheel(params, "gain", QT_TRANSLATE_NOOP("VideoFilters", "Gain"), 1, 0, 2, QT_TRANSLATE_NOOP("VideoFilters", "The highlights: the wheel pushes them towards a colour, the level scales white"));
    wheel(params, "offset", QT_TRANSLATE_NOOP("VideoFilters", "Offset"), 0, -1, 1, QT_TRANSLATE_NOOP("VideoFilters", "Everything at once: the wheel shifts the whole picture's colour, the level its brightness"));
    params.push_back(number("contrast", QT_TRANSLATE_NOOP("VideoFilters", "Contrast"), "%", 0, -100, 100, 1));
    params.push_back(number("pivot", QT_TRANSLATE_NOOP("VideoFilters", "Pivot"), "", 0.46, 0, 1, 0.01,
                            QT_TRANSLATE_NOOP("VideoFilters", "The brightness contrast turns about: what is darker gets darker, what is brighter brighter")));
    params.push_back(number("saturation", QT_TRANSLATE_NOOP("VideoFilters", "Saturation"), "%", 100, 0, 200, 1, QT_TRANSLATE_NOOP("VideoFilters", "0 is grey, 100 unchanged")));
    return params;
}

#define TR(text) QT_TRANSLATE_NOOP("VideoFilters", text)
const std::vector<Def> &definitions() {
    static const std::vector<Def> defs{
        {T::Colour, "colour", TR("Colour correction"), TR("Gamma, contrast, brightness, saturation and hue on display values, then a multiply and an add colour, and opacity."),
         {number("gamma", TR("Gamma"), "", 0, -3, 3, 0.01, TR("Above zero lifts the mid-tones, below darkens them")),
          number("contrast", TR("Contrast"), "", 0, -1, 1, 0.01), number("brightness", TR("Brightness"), "", 0, -1, 1, 0.01),
          number("saturation", TR("Saturation"), "", 0, -1, 4, 0.01, TR("-1 is grey, 0 unchanged")),
          number("hue", TR("Hue shift"), "°", 0, -180, 180, 1), number("opacity", TR("Opacity"), "%", 100, 0, 100, 1),
          colour("multiply", TR("Multiply colour"), "#ffffff"), colour("add", TR("Add colour"), "#000000")}},
        {T::ColourWheels, "colourWheels", TR("Colour wheels"),
         TR("Grade with lift, gamma and gain: push the shadows, mid-tones and highlights each towards a colour and set their level; offset moves them all. Then contrast about a pivot, and saturation. Watch the scopes while you do."),
         wheelParams()},
        {T::Sharpen, "sharpen", TR("Sharpen"), TR("Lifts edges against their four neighbours."),
         {number("amount", TR("Amount"), "%", 10, 0, 100, 1)}},
        {T::ChromaKey, "chromaKey", TR("Chroma key"), TR("Takes out a green, blue or magenta screen. The backdrop's colour is found in the picture; hair and motion keep their soft edges, and the screen's colour is taken out of them and of what it lit."),
         {choice("keyType", TR("Screen"), "green", {{"green", TR("Green")}, {"blue", TR("Blue")}, {"magenta", TR("Magenta")}, {"custom", TR("Other")}}),
          colour("backdrop", TR("Backdrop colour"), "#3fcc4a"),
          number("tolerance", TR("Similarity"), "%", 15, 0, 60, 1, TR("How much weaker than the backdrop a colour can be and still be taken out: more for uneven lighting")),
          number("softness", TR("Edge softness"), "%", 80, 5, 100, 1, TR("How gradually edges fade: lower is crisper, higher keeps more of hair and motion blur")),
          number("spillRemoval", TR("Spill reduction"), "%", 100, 0, 100, 1, TR("How much of the screen's colour is taken out of the subject and its edges"))}},
        {T::ColourKey, "colourKey", TR("Colour key"), TR("Makes one colour transparent, brightness included."),
         {choice("keyType", TR("Key colour"), "green", {{"green", TR("Green")}, {"blue", TR("Blue")}, {"red", TR("Red")}, {"magenta", TR("Magenta")}, {"custom", TR("Custom")}}),
          when(colour("keyColour", TR("Custom colour"), "#00ff00"), "keyType", "custom"),
          number("similarity", TR("Similarity"), "%", 10, 0, 100, 1), number("smoothness", TR("Smoothness"), "%", 5, 0, 100, 1)}},
        {T::LumaKey, "lumaKey", TR("Luma key"), TR("Keeps what is between two brightnesses; darker and brighter parts fade out."),
         {number("min", TR("Lowest kept"), "%", 0, 0, 100, 1), number("minSmooth", TR("Fade below"), "%", 0, 0, 100, 1),
          number("max", TR("Highest kept"), "%", 100, 0, 100, 1), number("maxSmooth", TR("Fade above"), "%", 0, 0, 100, 1)}},
        {T::Lut, "lut", TR("Apply LUT"), TR("A colour grade from a .cube file (1D or 3D) or a LUT picture: a Hald or tiled square, or a strip."),
         {file("file", TR("LUT file")), number("amount", TR("Amount"), "%", 100, 0, 100, 1),
          choice("interpolation", TR("Interpolation"), "trilinear", {{"trilinear", TR("Trilinear")}, {"tetrahedral", TR("Tetrahedral")}}),
          choice("pictureLayout", TR("Square picture"), "hald", {{"hald", TR("Hald")}, {"tiles", TR("Tiles")}})}},
        {T::Mask, "mask", TR("Image mask/blend"), TR("A picture stretched over the source: its alpha or brightness limits the source's alpha, or its colour multiplies, adds or subtracts."),
         {file("file", TR("Picture")),
          choice("mode", TR("Type"), "alpha", {{"alpha", TR("Alpha mask (alpha channel)")}, {"luma", TR("Alpha mask (colour channel)")},
                                               {"multiply", TR("Blend (multiply)")}, {"add", TR("Blend (add)")}, {"subtract", TR("Blend (subtract)")}}),
          colour("colour", TR("Colour"), "#ffffff"), number("opacity", TR("Opacity"), "%", 100, 0, 100, 1)}},
        {T::Crop, "crop", TR("Crop/pad"), TR("Cuts pixels off the edges; a negative amount adds transparent padding."),
         {toggle("relative", TR("From each edge"), true, TR("Off: a box at a position and size")),
          number("left", TR("Left"), "px", 0, -8192, 8192, 1), number("top", TR("Top"), "px", 0, -8192, 8192, 1),
          when(number("right", TR("Right"), "px", 0, -8192, 8192, 1), "relative", "true"),
          when(number("bottom", TR("Bottom"), "px", 0, -8192, 8192, 1), "relative", "true"),
          when(number("width", TR("Width"), "px", 1920, 1, 16384, 1), "relative", "false"),
          when(number("height", TR("Height"), "px", 1080, 1, 16384, 1), "relative", "false")}},
        {T::Scale, "scale", TR("Scaling/aspect ratio"), TR("Resamples the picture to a size; 0 keeps a side, or follows the shape when the other is set."),
         {number("width", TR("Width"), "px", 0, 0, 16384, 1), number("height", TR("Height"), "px", 0, 0, 16384, 1),
          choice("sampling", TR("Sampling"), "bicubic", {{"point", TR("Point")}, {"bilinear", TR("Bilinear")}, {"bicubic", TR("Bicubic")},
                                                         {"lanczos", TR("Lanczos")}, {"area", TR("Area")}}),
          choice("fit", TR("Shape"), "stretch", {{"stretch", TR("Stretch")}, {"fit", TR("Fit inside (transparent bars)")}, {"fill", TR("Fill (crop the sides)")}})},
         false,
         {{"3840×2160", 3840, 2160}, {"2560×1440", 2560, 1440}, {"1920×1080", 1920, 1080}, {"1600×900", 1600, 900}, {"1280×720", 1280, 720},
          {"1024×576", 1024, 576}, {"960×540", 960, 540}, {"854×480", 854, 480}, {"640×360", 640, 360}, {"1080×1920", 1080, 1920},
          {"1080×1080", 1080, 1080}, {"1440×1080", 1440, 1080}}},
        {T::Scroll, "scroll", TR("Scroll"), TR("Moves the picture continuously; looping repeats it edge to edge, otherwise it crosses and comes back."),
         {number("speedX", TR("Across"), "px/s", 0, -4000, 4000, 1, TR("Positive moves right")),
          number("speedY", TR("Down"), "px/s", 0, -4000, 4000, 1, TR("Positive moves down")),
          number("width", TR("Shown width"), "px", 0, 0, 16384, 1, TR("0 keeps the picture's")),
          number("height", TR("Shown height"), "px", 0, 0, 16384, 1, TR("0 keeps the picture's")), toggle("loop", TR("Loop"), true)}},
        {T::Blur, "blur", TR("Blur"), TR("Softens the picture: a Gaussian reaching this many pixels."),
         {number("radius", TR("Radius"), "px", 4, 0, 64, 0.5)}},
        {T::Shadow, "shadow", TR("Drop shadow"), TR("A soft copy of the picture's shape, moved and coloured, under it. It stays inside the picture: pad with Crop/pad first to give it room."),
         {number("offsetX", TR("Across"), "px", 8, -512, 512, 1), number("offsetY", TR("Down"), "px", 8, -512, 512, 1),
          number("radius", TR("Softness"), "px", 8, 0, 64, 0.5), colour("colour", TR("Colour"), "#000000"), number("opacity", TR("Opacity"), "%", 60, 0, 100, 1)}},
        {T::RenderDelay, "renderDelay", TR("Render delay"), TR("Shows every drawn picture this much later. Long delays at large sizes hold at most 1 GB of pictures."),
         {number("delayMs", TR("Delay"), "ms", 0, 0, 500, 1)}},
        {T::VideoDelay, "videoDelay", TR("Video delay"), TR("Shows the source's pictures this much later, to meet its sound. Long delays at large sizes hold at most 1 GB of pictures."),
         {number("delayMs", TR("Delay"), "ms", 0, 0, 5000, 1)}, true},
        {T::ToneMap, "toneMap", TR("HDR tone map"), TR("Brings an HDR source's highlights down into SDR, from its brightest light to SDR white, keeping each colour's hue. For an SDR recording of HDR video or games."),
         {choice("curve", TR("Curve"), "bt2390", {{"bt2390", TR("Roll-off (BT.2390)")}, {"reinhard", TR("Reinhard")}, {"clip", TR("Clip")}}),
          number("peak", TR("Source peak"), "nits", 1000, 100, 10000, 10, TR("The brightest the source gets; it lands on SDR white"))}},
        {T::SdrOnHdr, "sdrOnHdr", TR("SDR on HDR"), TR("How bright an SDR source shows in an HDR picture: its white at a level of its own instead of the profile's SDR white."),
         {toggle("useMultiplier", TR("Multiply instead"), false, TR("On: scale the light by a factor rather than set its white")),
          when(number("white", TR("White level"), "nits", 203, 10, 10000, 1, TR("Where the source's white shows")), "useMultiplier", "false"),
          when(number("multiplier", TR("Multiplier"), "×", 1, 0, 50, 0.05), "useMultiplier", "true")}},
        {T::RemoveBackground, "removeBackground", TR("Remove background (AI)"), TR("Finds the people in the picture and takes everything else away, with no green screen. It knows people only, not objects or pets, and works best on one or two people from the waist up."),
         {choice("background", TR("Background"), "remove", {{"remove", TR("Take away")}, {"blur", TR("Blur")}},
                 TR("Take the background away (see-through), or keep it blurred behind you, as on a video call")),
          when(number("blurAmount", TR("Blur"), "%", 3, 0.5, 10, 0.5, TR("How much the background is blurred, as a share of the picture's height")), "background", "blur"),
          choice("quality", TR("Quality"), "fast", {{"fast", TR("Fast")}, {"balanced", TR("Balanced")}, {"best", TR("Best")}},
                 TR("How closely it looks: Best follows edges more closely and costs the graphics card more")),
          choice("spread", TR("Spread over"), "2", {{"1", TR("1 frame")}, {"2", TR("2 frames")}, {"3", TR("3 frames")}},
                 TR("Shares the work out over more frames, so each costs less; the outline then follows movement a frame or two later")),
          number("tighten", TR("Tighten edges"), "%", 0, 0, 100, 1, TR("Narrows the soft edge round people: raise it if a rim of the background shows"))}},
        {T::LightWrap, "lightWrap", TR("Light wrap"), TR("Lets the light of whatever is behind this source spill a little way over its edges, so a keyed person looks lit by the scene they are put in. Put it after Chroma key or Remove background (AI). It needs what is behind, so the preview shows the studio while it is selected (In the studio)."),
         {choice("style", TR("Style"), "light", {{"light", TR("Add light")}, {"blend", TR("Blend colour")}},
                 TR("Add light brightens the edges with the light behind; Blend colour gives them the colours behind, darker or lighter, which suits dark scenes")),
          number("width", TR("Width"), "%", 5, 0.5, 25, 0.5, TR("How far in from the edges it reaches, as a share of the studio's height")),
          number("strength", TR("Strength"), "%", 100, 0, 300, 1, TR("How much of the scene reaches the edges: 100 gives the very edge the full colour behind"))}},
    };
    return defs;
}
#undef TR

QString tr(const char *text) { return QCoreApplication::translate("VideoFilters", text); }

const Def *definition(const QString &type) {
    const auto &defs = definitions();
    const auto it = std::find_if(defs.begin(), defs.end(), [&](const Def &d) { return type == QLatin1String(d.key); });
    return it == defs.end() ? nullptr : &*it;
}

engine::Color colourOf(const QVariant &value) {
    const QColor c(value.toString());
    return {float(c.redF()), float(c.greenF()), float(c.blueF()), 1.f};
}

// Files by path, size and time: re-read when they change, kept while few.
template <class V> struct FileCache {
    std::mutex mutex;
    QHash<QString, std::shared_ptr<const V>> entries;
    std::shared_ptr<const V> get(const QString &path, const QString &variant, const std::function<std::shared_ptr<const V>(QString *)> &load, QString *error) {
        const QFileInfo info(path);
        // A FIFO would never open, and a device might never end.
        if (info.exists() && !info.isFile()) {
            if (error) *error = QCoreApplication::translate("VideoFilters", "not an ordinary file");
            return nullptr;
        }
        const QString key = info.absoluteFilePath() + QLatin1Char('|') + variant + QLatin1Char('|') + QString::number(info.size()) +
                            QLatin1Char('|') + QString::number(info.lastModified().toMSecsSinceEpoch());
        {
            std::lock_guard lock(mutex);
            if (const auto it = entries.constFind(key); it != entries.constEnd()) return it.value();
        }
        auto value = load(error);
        if (value) {
            std::lock_guard lock(mutex);
            if (entries.size() >= 16) entries.clear();
            entries.insert(key, value);
        }
        return value;
    }
};
}  // namespace

// Where each kind sits in the Add filter menu, and the menu's order of groups.
static std::pair<int, const char *> groupOf(T type) {
    switch (type) {
    case T::ChromaKey: case T::ColourKey: case T::LumaKey: case T::RemoveBackground: case T::Mask: return {0, QT_TRANSLATE_NOOP("VideoFilters", "Keying")};
    case T::Colour: case T::ColourWheels: case T::Lut: case T::ToneMap: case T::SdrOnHdr: return {1, QT_TRANSLATE_NOOP("VideoFilters", "Colour")};
    case T::Crop: case T::Scale: case T::Scroll: return {2, QT_TRANSLATE_NOOP("VideoFilters", "Size and position")};
    case T::Sharpen: case T::Blur: case T::Shadow: case T::LightWrap: return {3, QT_TRANSLATE_NOOP("VideoFilters", "Look")};
    default: return {4, QT_TRANSLATE_NOOP("VideoFilters", "Timing")};
    }
}

QVariantList VideoFilterSettings::catalog() {
    QVariantList list;
    for (const Def &d : definitions()) {
        QVariantList params;
        for (const Param &p : d.params) {
            static const char *kinds[] = {"number", "choice", "colour", "file", "toggle"};
            QVariantMap m{{"key", p.key}, {"label", tr(p.label)}, {"unit", QString::fromUtf8(p.unit)}, {"kind", kinds[int(p.kind)]},
                          {"minimum", p.minimum}, {"maximum", p.maximum}, {"step", p.step}, {"tip", tr(p.tip)}};
            if (p.kind == Kind::Number) m["initial"] = p.initial;
            else if (p.kind == Kind::Toggle) m["initial"] = p.initial != 0;
            else m["initial"] = QString::fromUtf8(p.text);
            QVariantList options;
            for (const Option &o : p.options) options.append(QVariantMap{{"value", o.value}, {"label", tr(o.label)}});
            m["options"] = options;
            if (p.kind == Kind::File)
                m["fileTypes"] = d.type == T::Lut ? QStringList{"*.cube", "*.png", "*.tif", "*.tiff"}
                                                  : QStringList{"*.png", "*.jpg", "*.jpeg", "*.webp", "*.bmp", "*.gif", "*.tif", "*.tiff"};
            if (p.showKey) m["showIf"] = QVariantMap{{p.showKey, QString::fromLatin1(p.showValue)}};
            if (p.wheel) m["wheel"] = QVariantMap{{"name", QString::fromLatin1(p.wheel)}, {"axis", QString::fromLatin1(p.axis)}};
            params.append(m);
        }
        QVariantList presets;
        for (const Preset &p : d.presets) presets.append(QVariantMap{{"label", QString::fromUtf8(p.label)}, {"values", QVariantMap{{"width", p.width}, {"height", p.height}}}});
        const auto group = groupOf(d.type);
        list.append(QVariantMap{{"type", d.key}, {"label", tr(d.label)}, {"description", tr(d.description)}, {"async", d.async},
                                {"group", tr(group.second)}, {"groupOrder", group.first}, {"presets", presets}, {"parameters", params}});
    }
    return list;
}

QVariantList VideoFilterSettings::normalized(const QVariantList &filters) {
    QVariantList list;
    QSet<QString> ids;
    for (const QVariant &value : filters) {
        if (list.size() == 32) break;
        const QVariantMap input = value.toMap();
        const QString type = input.value("type").toString();
        const Def *d = definition(type);
        if (!d) continue;
        QString id = input.value("id").toString().left(128);
        if (id.isEmpty() || ids.contains(id)) {
            id = type + QString::number(list.size());
            while (ids.contains(id)) id += QLatin1Char('_');
        }
        ids.insert(id);
        QVariantMap item{{"id", id}, {"type", type}, {"enabled", input.value("enabled", true).toBool()}};
        for (const Param &p : d->params) {
            const QVariant v = input.value(p.key);
            switch (p.kind) {
            case Kind::Number: {
                bool ok = false;
                const double n = v.toDouble(&ok);
                item.insert(p.key, ok && std::isfinite(n) ? std::clamp(n, p.minimum, p.maximum) : p.initial);
                break;
            }
            case Kind::Toggle: item.insert(p.key, v.isValid() ? v.toBool() : p.initial != 0); break;
            case Kind::Colour: {
                const QColor c(v.toString());
                item.insert(p.key, c.isValid() ? c.name(QColor::HexRgb) : QString::fromLatin1(p.text));
                break;
            }
            case Kind::Choice: {
                const QString s = v.toString();
                const bool known = std::any_of(p.options.begin(), p.options.end(), [&](const Option &o) { return s == QLatin1String(o.value); });
                item.insert(p.key, known ? s : QString::fromLatin1(p.text));
                break;
            }
            case Kind::File: item.insert(p.key, v.toString().left(4096)); break;
            }
        }
        list.append(item);
    }
    return list;
}

std::shared_ptr<const engine::LookupTable> VideoFilterSettings::loadLookupTable(const QString &path, bool tiles, QString *error) {
    static FileCache<engine::LookupTable> cache;
    auto fail = [&](const QString &why) { if (error) *error = why; return std::shared_ptr<const engine::LookupTable>(); };
    if (path.isEmpty()) return fail(QCoreApplication::translate("VideoFilters", "no LUT file chosen"));
    return cache.get(path, tiles ? QStringLiteral("tiles") : QStringLiteral("hald"), [&](QString *why) -> std::shared_ptr<const engine::LookupTable> {
        auto table = std::make_shared<engine::LookupTable>();
        auto bad = [&](const char *text) { if (why) *why = QCoreApplication::translate("VideoFilters", text); return nullptr; };
        if (path.endsWith(QLatin1String(".cube"), Qt::CaseInsensitive)) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return bad("the LUT file cannot be read");
            int size1 = 0, size3 = 0;
            QTextStream in(&f);
            while (!in.atEnd()) {
                const QString line = in.readLine().trimmed();
                if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
                // Spaces or tabs between the numbers, as files are written.
                static const QRegularExpression space(QStringLiteral("\\s+"));
                const QStringList w = line.split(space, Qt::SkipEmptyParts);
                const QString word = w.first().toUpper();
                if (word == QLatin1String("LUT_3D_SIZE") && w.size() == 2) size3 = w[1].toInt();
                else if (word == QLatin1String("LUT_1D_SIZE") && w.size() == 2) size1 = w[1].toInt();
                else if ((word == QLatin1String("DOMAIN_MIN") || word == QLatin1String("DOMAIN_MAX")) && w.size() == 4)
                    for (int i = 0; i < 3; ++i) (word == QLatin1String("DOMAIN_MIN") ? table->domainMin : table->domainMax)[i] = w[i + 1].toFloat();
                else if ((word == QLatin1String("LUT_1D_INPUT_RANGE") || word == QLatin1String("LUT_3D_INPUT_RANGE")) && w.size() == 3)
                    for (int i = 0; i < 3; ++i) { table->domainMin[i] = w[1].toFloat(); table->domainMax[i] = w[2].toFloat(); }
                else if (word.at(0).isDigit() || word.at(0) == QLatin1Char('-') || word.at(0) == QLatin1Char('.') || word.at(0) == QLatin1Char('+')) {
                    if (w.size() != 3) return bad("the LUT file has a line that is not three numbers");
                    // No more than the largest table there can be (128³).
                    if (table->rgb.size() >= size_t(128) * 128 * 128 * 3) return bad("the LUT file does not have as many entries as its size says");
                    for (const QString &n : w) {
                        bool ok = false;
                        const float v = n.toFloat(&ok);
                        if (!ok || !std::isfinite(v)) return bad("the LUT file has a line that is not three numbers");
                        table->rgb.push_back(v);
                    }
                }
            }
            if ((size1 > 0) == (size3 > 0)) return bad("the LUT file must have one size, 1D or 3D");
            table->cube = size3 > 0;
            table->size = table->cube ? size3 : size1;
            if (table->size < 2 || (table->cube ? table->size > 128 : table->size > 16384)) return bad("the LUT file's size is not supported");
            const size_t entries = table->cube ? size_t(table->size) * table->size * table->size : size_t(table->size);
            if (table->rgb.size() != entries * 3) return bad("the LUT file does not have as many entries as its size says");
            for (int i = 0; i < 3; ++i) if (!(table->domainMax[i] > table->domainMin[i])) return bad("the LUT file's domain is empty");
            return table;
        }
        QImage image(path);
        if (image.isNull()) return bad("the LUT picture cannot be read");
        image = image.convertToFormat(QImage::Format_RGB32);
        const int w = image.width(), h = image.height();
        int n = 0, layout = 0;  // 0 Hald, 1 tiles, 2 strip across, 3 strip down
        if (w == h * h) { n = h; layout = 2; }
        else if (h == w * w) { n = w; layout = 3; }
        else if (w == h) {
            const int l = int(std::lround(std::cbrt(double(w))));
            if (l * l * l == w) { n = l * l; layout = tiles ? 1 : 0; }
        }
        if (n < 2 || n > 128) return bad("the LUT picture is not a Hald, tiled or strip LUT");
        table->size = n;
        table->rgb.resize(size_t(n) * n * n * 3);
        const int perRow = int(std::lround(std::sqrt(double(n))));
        for (int b = 0; b < n; ++b) for (int g = 0; g < n; ++g) for (int r = 0; r < n; ++r) {
            int x = 0, y = 0;
            const size_t i = (size_t(b) * n + g) * n + r;
            switch (layout) {
            case 0: x = int(i % size_t(w)); y = int(i / size_t(w)); break;
            case 1: x = r + (b % perRow) * n; y = g + (b / perRow) * n; break;
            case 2: x = r + b * n; y = g; break;
            default: x = r; y = g + b * n; break;
            }
            const QRgb c = image.pixel(x, y);
            table->rgb[i * 3] = qRed(c) / 255.f; table->rgb[i * 3 + 1] = qGreen(c) / 255.f; table->rgb[i * 3 + 2] = qBlue(c) / 255.f;
        }
        return table;
    }, error);
}

std::shared_ptr<const engine::FilterImage> VideoFilterSettings::loadImage(const QString &path, QString *error) {
    static FileCache<engine::FilterImage> cache;
    if (path.isEmpty()) {
        if (error) *error = QCoreApplication::translate("VideoFilters", "no picture chosen");
        return nullptr;
    }
    return cache.get(path, {}, [&](QString *why) -> std::shared_ptr<const engine::FilterImage> {
        QImage image(path);
        if (image.isNull()) {
            if (why) *why = QCoreApplication::translate("VideoFilters", "the picture cannot be read");
            return nullptr;
        }
        if (image.width() > 4096 || image.height() > 4096) image = image.scaled(4096, 4096, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        image = image.convertToFormat(QImage::Format_RGBA8888);
        auto out = std::make_shared<engine::FilterImage>();
        out->width = image.width();
        out->height = image.height();
        out->rgba.resize(size_t(out->width) * out->height * 4);
        for (int y = 0; y < out->height; ++y)
            std::copy_n(image.constScanLine(y), size_t(out->width) * 4, &out->rgba[size_t(y) * out->width * 4]);
        return out;
    }, error);
}

QString VideoFilterSettings::findBackdrop(const QImage &picture, const QString &screen, const QString &current, double x, double y, QString *why) {
    auto fail = [&](const char *text) { if (why) *why = QCoreApplication::translate("VideoFilters", text); return QString(); };
    if (picture.isNull()) return fail("there is no picture from this source yet");
    const QImage image = picture.convertToFormat(QImage::Format_RGB32);
    // Display values to their Cb/Cr, as the key reads them.
    auto chroma = [](QRgb c, float *y, float *cb, float *cr) {
        const float r = qRed(c) / 255.f, g = qGreen(c) / 255.f, b = qBlue(c) / 255.f;
        *y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        *cb = (b - *y) / 1.8556f;
        *cr = (r - *y) / 1.5748f;
    };
    auto hex = [](float y, float cb, float cr) {
        const float r = y + 1.5748f * cr, b = y + 1.8556f * cb, g = (y - 0.2126f * r - 0.0722f * b) / 0.7152f;
        return QColor::fromRgbF(std::clamp(r, 0.f, 1.f), std::clamp(g, 0.f, 1.f), std::clamp(b, 0.f, 1.f)).name(QColor::HexRgb);
    };
    if (x >= 0 && y >= 0) {
        // Picked: a small patch around the spot, so one noisy pixel does not decide it.
        const int cx = std::clamp(int(x * image.width()), 0, image.width() - 1), cy = std::clamp(int(y * image.height()), 0, image.height() - 1);
        const int r = std::max(2, image.width() / 400);
        float sy = 0, sb = 0, sr = 0;
        int n = 0;
        for (int j = std::max(0, cy - r); j <= std::min(image.height() - 1, cy + r); ++j)
            for (int i = std::max(0, cx - r); i <= std::min(image.width() - 1, cx + r); ++i) {
                float py, pb, pr;
                chroma(image.pixel(i, j), &py, &pb, &pr);
                sy += py; sb += pb; sr += pr; ++n;
            }
        sy /= n; sb /= n; sr /= n;
        if (std::hypot(sb, sr) < 0.03f) return fail("that spot is grey: pick a spot on the coloured screen");
        return hex(sy, sb, sr);
    }
    // The screen's hue: a strong colour of it, or the current backdrop's.
    QColor hue = screen == QLatin1String("blue") ? QColor(0, 0, 255) : screen == QLatin1String("magenta") ? QColor(255, 0, 255)
                 : screen == QLatin1String("custom") ? QColor(current) : QColor(0, 255, 0);
    if (!hue.isValid()) hue = QColor(0, 255, 0);
    float hy, hb, hr;
    chroma(hue.rgb(), &hy, &hb, &hr);
    const float hl = std::hypot(hb, hr);
    if (hl < 0.03f) return fail("the backdrop colour has no hue to look for");
    // Strong colours within 40° of it, binned by Cb/Cr; the fullest bin is the
    // backdrop (it fills the most of the picture), its colour their median.
    const QImage small = image.width() > 480 ? image.scaledToWidth(480, Qt::FastTransformation) : image;
    constexpr int kBins = 40;
    auto bin = [](float v) { return std::clamp(int((v + 0.6f) / 1.2f * kBins), 0, kBins - 1); };
    std::vector<int> counts(kBins * kBins, 0);
    struct Pixel { float y, cb, cr; int bin; };
    std::vector<Pixel> near;
    const size_t total = size_t(small.width()) * small.height();
    for (int j = 0; j < small.height(); ++j)
        for (int i = 0; i < small.width(); ++i) {
            float py, pb, pr;
            chroma(small.pixel(i, j), &py, &pb, &pr);
            const float l = std::hypot(pb, pr);
            if (l < 0.06f || (pb * hb + pr * hr) / (l * hl) < 0.766f) continue;
            const int b = bin(pb) * kBins + bin(pr);
            ++counts[b];
            near.push_back({py, pb, pr, b});
        }
    if (near.empty() || near.size() < total / 33) return fail("no screen of that colour fills enough of the picture");
    const int peak = int(std::max_element(counts.begin(), counts.end()) - counts.begin());
    const int pb0 = peak / kBins, pr0 = peak % kBins;
    std::vector<float> ys, cbs, crs;
    for (const Pixel &p : near)
        if (std::abs(p.bin / kBins - pb0) <= 1 && std::abs(p.bin % kBins - pr0) <= 1) { ys.push_back(p.y); cbs.push_back(p.cb); crs.push_back(p.cr); }
    auto median = [](std::vector<float> &v) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
    return hex(median(ys), median(cbs), median(crs));
}

bool VideoFilterSettings::readsFiles(const QVariantList &filters) {
    for (const QVariant &value : normalized(filters)) {
        const QVariantMap m = value.toMap();
        if (!m.value("enabled").toBool()) continue;
        const T type = definition(m.value("type").toString())->type;
        if (type == T::Lut || type == T::Mask) return true;
    }
    return false;
}

bool VideoFilterSettings::cutsOut(const QVariantList &filters) {
    for (const QVariant &value : normalized(filters)) {
        const QVariantMap m = value.toMap();
        if (m.value("enabled").toBool() && definition(m.value("type").toString())->type == T::RemoveBackground) return true;
    }
    return false;
}

std::vector<engine::VideoFilter> VideoFilterSettings::parse(const QVariantList &filters, QStringList *problems, bool loadFiles) {
    std::vector<engine::VideoFilter> chain;
    for (const QVariant &value : normalized(filters)) {
        const QVariantMap m = value.toMap();
        if (!m.value("enabled").toBool()) continue;
        const Def *d = definition(m.value("type").toString());
        auto num = [&](const char *key) { return float(m.value(key).toDouble()); };
        auto keyColour = [&](engine::Color fallback) {
            const QString k = m.value("keyType").toString();
            if (k == QLatin1String("custom")) return colourOf(m.value("keyColour"));
            if (k == QLatin1String("blue")) return engine::Color{0, 0, 1, 1};
            if (k == QLatin1String("red")) return engine::Color{1, 0, 0, 1};
            if (k == QLatin1String("magenta")) return engine::Color{1, 0, 1, 1};
            return fallback;
        };
        engine::VideoFilter f;
        f.type = d->type;
        if (!loadFiles && (d->type == T::Lut || d->type == T::Mask)) {
            if (problems) problems->append(tr(d->label) + QStringLiteral(": ") + QCoreApplication::translate("VideoFilters", "left out in safe mode"));
            continue;
        }
        switch (d->type) {
        case T::Colour:
            f.gamma = num("gamma"); f.contrast = num("contrast"); f.brightness = num("brightness"); f.saturation = num("saturation");
            f.hue = num("hue"); f.opacity = num("opacity") / 100.f; f.multiply = colourOf(m.value("multiply")); f.add = colourOf(m.value("add"));
            break;
        case T::ColourWheels:
            f.lift = {num("liftX"), num("liftY"), num("lift")};
            f.gammaWheel = {num("gammaX"), num("gammaY"), num("gamma")};
            f.gain = {num("gainX"), num("gainY"), num("gain")};
            f.offset = {num("offsetX"), num("offsetY"), num("offset")};
            f.contrast = num("contrast") / 100.f; f.pivot = num("pivot"); f.saturation = num("saturation") / 100.f - 1.f;
            break;
        case T::Sharpen: f.amount = num("amount") / 100.f; break;
        case T::ChromaKey:
            // Percentages of the distance from a pure colour to grey (about 0.5 to 0.6).
            f.key = colourOf(m.value("backdrop")); f.similarity = num("tolerance") / 100.f; f.smoothness = num("softness") / 100.f; f.spill = num("spillRemoval") / 100.f;
            break;
        case T::ColourKey:
            f.key = keyColour({0, 1, 0, 1}); f.similarity = num("similarity") / 100.f; f.smoothness = num("smoothness") / 100.f;
            break;
        case T::LumaKey:
            f.lumaMin = num("min") / 100.f; f.lumaMax = num("max") / 100.f; f.lumaMinSmooth = num("minSmooth") / 100.f; f.lumaMaxSmooth = num("maxSmooth") / 100.f;
            break;
        case T::Lut: {
            QString why;
            f.lut = loadLookupTable(m.value("file").toString(), m.value("pictureLayout").toString() == QLatin1String("tiles"), &why);
            if (!f.lut) { if (problems) problems->append(tr(d->label) + QStringLiteral(": ") + why); continue; }
            f.amount = num("amount") / 100.f; f.tetrahedral = m.value("interpolation").toString() == QLatin1String("tetrahedral");
            break;
        }
        case T::Mask: {
            QString why;
            f.image = loadImage(m.value("file").toString(), &why);
            if (!f.image) { if (problems) problems->append(tr(d->label) + QStringLiteral(": ") + why); continue; }
            static const QStringList modes{"alpha", "luma", "multiply", "add", "subtract"};
            f.mask = engine::MaskMode(std::max(0, int(modes.indexOf(m.value("mode").toString()))));
            f.tint = colourOf(m.value("colour")); f.opacity = num("opacity") / 100.f;
            break;
        }
        case T::Crop:
            f.absolute = !m.value("relative").toBool();
            f.left = int(num("left")); f.top = int(num("top")); f.right = int(num("right")); f.bottom = int(num("bottom"));
            f.width = int(num("width")); f.height = int(num("height"));
            break;
        case T::Scale: {
            static const QStringList samplings{"bilinear", "bicubic", "lanczos", "area", "point"};
            static const QStringList fits{"stretch", "fit", "fill"};
            f.width = int(num("width")); f.height = int(num("height"));
            f.sampling = engine::ScaleFilter(1 + std::max(0, int(samplings.indexOf(m.value("sampling").toString()))));
            f.fit = engine::ScaleFit(std::max(0, int(fits.indexOf(m.value("fit").toString()))));
            if (f.width <= 0 && f.height <= 0) continue;  // nothing to do
            break;
        }
        case T::Scroll:
            f.speedX = num("speedX"); f.speedY = num("speedY"); f.width = int(num("width")); f.height = int(num("height")); f.loop = m.value("loop").toBool();
            break;
        case T::Blur:
            f.radius = num("radius");
            if (f.radius <= 0.f) continue;
            break;
        case T::Shadow:
            f.offsetX = int(num("offsetX")); f.offsetY = int(num("offsetY")); f.radius = num("radius");
            f.shadow = colourOf(m.value("colour")); f.opacity = num("opacity") / 100.f;
            break;
        case T::RenderDelay:
        case T::VideoDelay:
            f.delayMs = num("delayMs");
            if (f.delayMs <= 0.f) continue;
            break;
        case T::ToneMap: {
            const QString curve = m.value("curve").toString();
            f.tone = curve == QLatin1String("reinhard") ? engine::ToneCurve::Reinhard
                   : curve == QLatin1String("clip") ? engine::ToneCurve::Clip : engine::ToneCurve::Bt2390;
            f.peakNits = num("peak");
            break;
        }
        case T::RemoveBackground: {
            static const QStringList qualities{"fast", "balanced", "best"};
            f.quality = std::max(0, int(qualities.indexOf(m.value("quality").toString())));
            f.tighten = num("tighten") / 100.f;
            f.spread = std::clamp(m.value("spread").toString().toInt(), 1, 3);
            f.blurBackground = m.value("background").toString() == QLatin1String("blur");
            f.radius = num("blurAmount") / 100.f;
            break;
        }
        case T::LightWrap:
            f.radius = num("width") / 100.f; f.opacity = num("strength") / 100.f;
            f.blendColour = m.value("style").toString() == QLatin1String("blend");
            if (f.radius <= 0.f || f.opacity <= 0.f) continue;
            break;
        case T::SdrOnHdr:
            f.useMultiplier = m.value("useMultiplier").toBool(); f.whiteNits = num("white"); f.multiplier = num("multiplier");
            break;
        }
        chain.push_back(std::move(f));
    }
    return chain;
}
