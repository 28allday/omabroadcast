#pragma once

#include <QImage>
#include <QQuickItem>

// Shows the latest preview frame the session produced. The image is
// uploaded as a scene-graph texture on each update.
class PreviewView : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(int imageWidth READ imageWidth NOTIFY imageChanged)
    Q_PROPERTY(int imageHeight READ imageHeight NOTIFY imageChanged)
    // The picture's transparency as grey (white solid, black clear): a key's matte.
    Q_PROPERTY(bool showAlpha READ showAlpha WRITE setShowAlpha NOTIFY showAlphaChanged)

public:
    explicit PreviewView(QQuickItem *parent = nullptr);

    int imageWidth() const { return m_image.width(); }
    int imageHeight() const { return m_image.height(); }
    bool showAlpha() const { return m_showAlpha; }
    void setShowAlpha(bool on);

public slots:
    void setImage(const QImage &image);

signals:
    void imageChanged();
    void showAlphaChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;

private:
    QImage m_image, m_shown;  // as given; as drawn
    bool m_dirty = false;
    bool m_showAlpha = false;
    void refreshShown();
};
