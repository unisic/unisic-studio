#include "VideoFrameItem.h"

#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QThread>

VideoFrameItem::VideoFrameItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

VideoFrameItem::~VideoFrameItem() = default;
// The texture is owned by the QSGSimpleTextureNode (setOwnsTexture): the scene
// graph tears the node down on the render thread and frees its texture there.
// Nothing to free here — deleting a QSGTexture off the render thread is a bug.

void VideoFrameItem::setFrame(const QImage &frame)
{
    {
        QMutexLocker lock(&m_mutex);
        // Detach to an independent copy: the producer's buffer is reused for the
        // next frame the moment this returns.
        m_pending = frame;
        m_pending.detach();
        m_hasPending = true;
    }
    requestRepaint();
}

void VideoFrameItem::requestRepaint()
{
    if (QThread::currentThread() == thread()) {
        // On the item's own thread (the export drives frames from here): mark
        // dirty synchronously so the very next renderControl.render() picks the
        // frame up in the SAME call stack.
        update();
    } else {
        // From a worker thread: hop to the GUI thread before touching the item.
        QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
    }
}

void VideoFrameItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        update(); // node rect follows the item; re-run updatePaintNode
}

QSGNode *VideoFrameItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);

    QImage frame;
    bool hasNew = false;
    {
        QMutexLocker lock(&m_mutex);
        if (m_hasPending) {
            frame = m_pending;
            m_hasPending = false;
            hasNew = true;
        }
    }

    // Nothing ever pushed and no node yet: draw nothing (transparent → the
    // composition background shows through).
    if (!node && (!hasNew || frame.isNull())) {
        return nullptr;
    }
    if (!node) {
        node = new QSGSimpleTextureNode;
        node->setOwnsTexture(true); // node deletes its texture on the render thread
        node->setFiltering(QSGTexture::Linear);
    }

    if (hasNew && !frame.isNull()) {
        // Upload the frame. QSGTexture has no public "re-upload into existing
        // texture" path, so each frame is uploaded via createTextureFromImage.
        // The node OWNS its texture (setOwnsTexture below): setTexture() frees the
        // previous one on the render thread — so we must NOT delete it a second
        // time here (that double-free crashed on the first frame that had a
        // predecessor).
        QSGTexture *tex = window()->createTextureFromImage(
            frame, QQuickWindow::TextureCanUseAtlas);
        node->setTexture(tex);
    }

    node->setRect(boundingRect());
    return node;
}
