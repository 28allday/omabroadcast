#include "preview_view.h"

#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>

PreviewView::PreviewView(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

void PreviewView::setImage(const QImage &image) {
    if (image.isNull()) return;
    const bool sizeChanged = image.size() != m_image.size();
    m_image = image;
    refreshShown();
    if (sizeChanged) emit imageChanged();
}

void PreviewView::setShowAlpha(bool on) {
    if (on == m_showAlpha) return;
    m_showAlpha = on;
    refreshShown();
    emit showAlphaChanged();
}

void PreviewView::refreshShown() {
    if (m_showAlpha && m_image.hasAlphaChannel()) {
        const QImage straight = m_image.convertToFormat(QImage::Format_ARGB32);
        m_shown = QImage(straight.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < straight.height(); ++y) {
            const QRgb *in = reinterpret_cast<const QRgb *>(straight.constScanLine(y));
            uchar *out = m_shown.scanLine(y);
            for (int x = 0; x < straight.width(); ++x) out[x] = uchar(qAlpha(in[x]));
        }
    } else {
        m_shown = m_image;
    }
    m_dirty = true;
    update();
}

QSGNode *PreviewView::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    auto *node = static_cast<QSGSimpleTextureNode *>(old);
    if (m_shown.isNull()) {
        delete node;
        return nullptr;
    }
    if (!node) {
        node = new QSGSimpleTextureNode;
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
        m_dirty = true;
    }
    if (m_dirty) {
        node->setTexture(window()->createTextureFromImage(m_shown));
        m_dirty = false;
    }
    node->setRect(boundingRect());
    return node;
}
