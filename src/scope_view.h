#pragma once

#include <QColor>
#include <QImage>
#include <QQuickPaintedItem>
#include <array>
#include <vector>

// Scopes of a picture's display values, as a grading tool shows them:
// "waveform" (brightness, BT.709 luma, by column), "parade" (red, green and
// blue side by side), "vectorscope" (colour as Cb/Cr: angle is hue, distance
// saturation, with the 75 % colour bars' targets and the skin-tone line) or
// "histogram". Clear pixels are left out. Fed the pictures a preview shows.
class ScopeView : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QColor backgroundColor MEMBER m_background WRITE setBackgroundColor NOTIFY coloursChanged)
    Q_PROPERTY(QColor gridColor MEMBER m_grid WRITE setGridColor NOTIFY coloursChanged)
    Q_PROPERTY(QColor textColor MEMBER m_text WRITE setTextColor NOTIFY coloursChanged)
    Q_PROPERTY(bool hasPicture READ hasPicture NOTIFY hasPictureChanged)

public:
    explicit ScopeView(QQuickItem *parent = nullptr);

    QString mode() const { return m_mode; }
    void setMode(const QString &mode);
    bool hasPicture() const { return m_samples > 0; }
    void setBackgroundColor(const QColor &c) { m_background = c; emit coloursChanged(); update(); }
    void setGridColor(const QColor &c) { m_grid = c; emit coloursChanged(); update(); }
    void setTextColor(const QColor &c) { m_text = c; emit coloursChanged(); update(); }
    void paint(QPainter *painter) override;

    // What the scopes count: per column (at most kColumns) and
    // display value 0..255 of luma or of each channel; Cb/Cr in a
    // kVector × kVector grid (±0.5 across); per value of each channel and luma.
    static constexpr int kColumns = 512, kVector = 256;
    struct Counts {
        int columns = 0;
        std::vector<uint32_t> luma, red, green, blue;  // columns × 256
        std::vector<uint32_t> vector;                  // kVector × kVector, row 0 at Cr +0.5
        std::array<std::array<uint32_t, 256>, 4> histogram{};  // R, G, B, luma
        int samples = 0;
    };
    static Counts count(const QImage &picture);

public slots:
    void setImage(const QImage &image);

signals:
    void modeChanged();
    void coloursChanged();
    void hasPictureChanged();

private:
    void render();
    QString m_mode = QStringLiteral("waveform");
    QColor m_background{0x10, 0x12, 0x16}, m_grid{0x50, 0x56, 0x60}, m_text{0xa0, 0xa6, 0xb0};
    Counts m_counts;
    QImage m_plot;  // the scope's traces, drawn under the graticule
    int m_samples = 0;
};
