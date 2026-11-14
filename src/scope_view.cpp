#include "scope_view.h"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace {
// About this many pixels counted of each picture, every few taken across a big one.
constexpr int kMostSamples = 250000;
// Cb and Cr of a display-encoded colour (BT.709), each −0.5..0.5.
void chroma(double r, double g, double b, double *cb, double *cr) {
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    *cb = (b - y) / 1.8556;
    *cr = (r - y) / 1.5748;
}
// How bright a trace with `count` hits shows, against `full` for a solid
// one; a single hit still shows, so a few pixels are not lost.
float strength(double count, double full) {
    if (count <= 0) return 0.f;
    const double v = std::pow(std::log1p(count) / std::log1p(std::max(full, 2.0)), 0.7);
    return float(std::clamp(0.3 + 0.7 * v, 0.0, 1.0));
}
}  // namespace

ScopeView::ScopeView(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
}

void ScopeView::setMode(const QString &mode) {
    if (mode == m_mode) return;
    m_mode = mode;
    emit modeChanged();
    render();
    update();
}

ScopeView::Counts ScopeView::count(const QImage &picture) {
    Counts c;
    if (picture.isNull()) return c;
    const QImage image = picture.format() == QImage::Format_ARGB32 ? picture : picture.convertToFormat(QImage::Format_ARGB32);
    const int w = image.width(), h = image.height();
    const int step = std::max(1, int(std::ceil(std::sqrt(double(w) * h / kMostSamples))));
    c.columns = std::min(kColumns, w);
    for (auto *v : {&c.luma, &c.red, &c.green, &c.blue}) v->assign(size_t(c.columns) * 256, 0);
    c.vector.assign(size_t(kVector) * kVector, 0);
    for (int y = 0; y < h; y += step) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < w; x += step) {
            const QRgb p = row[x];
            if (qAlpha(p) < 16) continue;  // clear: not part of the picture
            const int r = qRed(p), g = qGreen(p), b = qBlue(p);
            const int l = (54 * r + 183 * g + 19 * b) >> 8;  // BT.709 weights in 256ths
            const size_t col = size_t(x) * c.columns / w * 256;
            ++c.luma[col + l]; ++c.red[col + r]; ++c.green[col + g]; ++c.blue[col + b];
            ++c.histogram[0][r]; ++c.histogram[1][g]; ++c.histogram[2][b]; ++c.histogram[3][l];
            double cb, cr;
            chroma(r / 255.0, g / 255.0, b / 255.0, &cb, &cr);
            const int vx = std::clamp(int((cb + 0.5) * kVector), 0, kVector - 1);
            const int vy = std::clamp(int((0.5 - cr) * kVector), 0, kVector - 1);
            ++c.vector[size_t(vy) * kVector + vx];
            ++c.samples;
        }
    }
    return c;
}

void ScopeView::setImage(const QImage &image) {
    const bool had = hasPicture();
    m_counts = count(image);
    m_samples = m_counts.samples;
    if (had != hasPicture()) emit hasPictureChanged();
    render();
    update();
}

// The traces as a picture: one pixel per column and value (waveform,
// parade) or per Cb/Cr cell (vectorscope); the histogram is drawn as paths.
void ScopeView::render() {
    const Counts &c = m_counts;
    if (c.samples == 0 || c.columns == 0) { m_plot = QImage(); return; }
    // A column's pixels all on one value show at full strength.
    const double perColumn = double(c.samples) / c.columns;
    if (m_mode == QLatin1String("waveform") || m_mode == QLatin1String("parade")) {
        const bool parade = m_mode == QLatin1String("parade");
        const int sections = parade ? 3 : 1;
        m_plot = QImage(c.columns * sections, 256, QImage::Format_ARGB32_Premultiplied);
        m_plot.fill(Qt::transparent);
        const std::vector<uint32_t> *traces[3] = {parade ? &c.red : &c.luma, &c.green, &c.blue};
        const QColor tints[3] = {parade ? QColor(255, 90, 80) : QColor(225, 235, 225), QColor(90, 235, 110), QColor(90, 140, 255)};
        for (int s = 0; s < sections; ++s) {
            const std::vector<uint32_t> &t = *traces[s];
            const QColor tint = tints[s];
            for (int x = 0; x < c.columns; ++x)
                for (int v = 0; v < 256; ++v) {
                    const float a = strength(t[size_t(x) * 256 + v], perColumn * 0.25);
                    if (a <= 0.f) continue;
                    m_plot.setPixel(s * c.columns + x, 255 - v, qRgba(int(tint.red() * a), int(tint.green() * a), int(tint.blue() * a), int(255 * a)));
                }
        }
    } else if (m_mode == QLatin1String("vectorscope")) {
        m_plot = QImage(kVector, kVector, QImage::Format_ARGB32_Premultiplied);
        m_plot.fill(Qt::transparent);
        const double full = std::max(4.0, c.samples / 3000.0);
        // Each cell with half of its neighbours' hits, so one flat colour shows as a dot, not a speck.
        auto hits = [&](int x, int y) { return (x < 0 || y < 0 || x >= kVector || y >= kVector) ? 0.0 : double(c.vector[size_t(y) * kVector + x]); };
        for (int y = 0; y < kVector; ++y)
            for (int x = 0; x < kVector; ++x) {
                const double near = hits(x, y) + 0.5 * (hits(x - 1, y) + hits(x + 1, y) + hits(x, y - 1) + hits(x, y + 1)) +
                                    0.25 * (hits(x - 1, y - 1) + hits(x + 1, y - 1) + hits(x - 1, y + 1) + hits(x + 1, y + 1));
                const float a = strength(near, full);
                if (a <= 0.f) continue;
                // Each cell in its own colour: Cb/Cr at mid brightness, brightened to the brightest channel.
                const double cb = (x + 0.5) / kVector - 0.5, cr = 0.5 - (y + 0.5) / kVector;
                double r = 0.5 + 1.5748 * cr, b = 0.5 + 1.8556 * cb, g = (0.5 - 0.2126 * r - 0.0722 * b) / 0.7152;
                const double top = std::max({r, g, b, 1e-6}), lo = std::min({r, g, b, 0.0});
                r = (r - lo) / (top - lo); g = (g - lo) / (top - lo); b = (b - lo) / (top - lo);
                // Towards white in the middle, so grey reads as grey.
                const double mix = std::min(1.0, std::hypot(cb, cr) * 8.0);
                r = 1.0 + (r - 1.0) * mix; g = 1.0 + (g - 1.0) * mix; b = 1.0 + (b - 1.0) * mix;
                m_plot.setPixel(x, y, qRgba(int(255 * r * a), int(255 * g * a), int(255 * b * a), int(255 * a)));
            }
    } else {
        m_plot = QImage();
    }
}

void ScopeView::paint(QPainter *painter) {
    const QRectF all(0, 0, width(), height());
    painter->fillRect(all, m_background);
    QFont font = painter->font();
    font.setPixelSize(10);
    painter->setFont(font);
    const QPen grid(m_grid, 1);
    if (m_samples == 0) return;

    if (m_mode == QLatin1String("vectorscope")) {
        const double side = std::min(width(), height()) - 8;
        const QRectF box((width() - side) / 2, (height() - side) / 2, side, side);
        const QPointF centre = box.center();
        const double radius = side / 2;
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        painter->drawImage(box, m_plot);
        painter->setPen(grid);
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(centre, radius, radius);
        painter->drawLine(QPointF(centre.x() - radius, centre.y()), QPointF(centre.x() + radius, centre.y()));
        painter->drawLine(QPointF(centre.x(), centre.y() - radius), QPointF(centre.x(), centre.y() + radius));
        // The skin-tone line: 123° round from the Cb axis, where faces of every complexion fall.
        const double skin = 123.0 * M_PI / 180.0;
        QPen skinPen(m_grid, 1, Qt::DashLine);
        painter->setPen(skinPen);
        painter->drawLine(centre, centre + QPointF(std::cos(skin), -std::sin(skin)) * radius);
        // The 75 % colour bars' targets.
        struct Target { const char *name; double r, g, b; };
        static const Target targets[] = {{"R", .75, 0, 0}, {"Yl", .75, .75, 0}, {"G", 0, .75, 0}, {"Cy", 0, .75, .75}, {"B", 0, 0, .75}, {"Mg", .75, 0, .75}};
        painter->setPen(QPen(m_text, 1));
        for (const Target &t : targets) {
            double cb, cr;
            chroma(t.r, t.g, t.b, &cb, &cr);
            const QPointF at = centre + QPointF(cb, -cr) * (side);
            painter->drawRect(QRectF(at - QPointF(4, 4), QSizeF(8, 8)));
            const QPointF out = centre + QPointF(cb, -cr) * (side) * 1.14;
            painter->drawText(QRectF(out - QPointF(10, 7), QSizeF(20, 14)), Qt::AlignCenter, QString::fromLatin1(t.name));
        }
        return;
    }

    // Waveform, parade and histogram: a plot with a scale on the left.
    const double left = 26, top = 6, bottom = 6, right = 6;
    const QRectF plot(left, top, std::max(1.0, width() - left - right), std::max(1.0, height() - top - bottom));
    if (m_mode == QLatin1String("histogram")) {
        const Counts &c = m_counts;
        // Scaled to the tallest value inside, so a clipped end does not flatten the rest.
        uint32_t tallest = 1;
        for (int k = 0; k < 4; ++k)
            for (int v = 1; v < 255; ++v) tallest = std::max(tallest, c.histogram[k][v]);
        auto path = [&](int k) {
            QPainterPath p(QPointF(plot.left(), plot.bottom()));
            for (int v = 0; v < 256; ++v) {
                const double x = plot.left() + plot.width() * (v + 0.5) / 256.0;
                p.lineTo(x, plot.bottom() - plot.height() * std::min(1.0, double(c.histogram[k][v]) / tallest));
            }
            p.lineTo(plot.right(), plot.bottom());
            p.closeSubpath();
            return p;
        };
        painter->setPen(grid);
        for (int i = 0; i <= 4; ++i) {
            const double x = plot.left() + plot.width() * i / 4.0;
            painter->drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }
        painter->setPen(Qt::NoPen);
        painter->setCompositionMode(QPainter::CompositionMode_Plus);
        const QColor fills[3] = {QColor(200, 50, 45), QColor(45, 180, 70), QColor(50, 90, 210)};
        for (int k = 0; k < 3; ++k) {
            painter->setBrush(fills[k]);
            painter->drawPath(path(k));
        }
        painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(QColor(230, 230, 230), 1));
        painter->drawPath(path(3));
        return;
    }
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(plot, m_plot);
    // Lines at 0, 25, 50, 75 and 100 % of the display range.
    painter->setPen(grid);
    for (int i = 0; i <= 4; ++i) {
        const double y = plot.bottom() - plot.height() * i / 4.0;
        painter->drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter->save();
        painter->setPen(QPen(m_text, 1));
        painter->drawText(QRectF(0, y - 7, left - 4, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(i * 25));
        painter->restore();
    }
    if (m_mode == QLatin1String("parade"))
        for (int s = 1; s < 3; ++s) {
            const double x = plot.left() + plot.width() * s / 3.0;
            painter->drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }
}
