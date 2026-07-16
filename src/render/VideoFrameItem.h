#pragma once
#include <QImage>
#include <QMutex>
#include <QQuickItem>
#include <qqmlregistration.h>

class QSGTexture;

// A QQuickItem that displays CPU-pushed video frames as a scene-graph texture.
// It is the bridge between the decoder (which produces QImages off the GUI
// thread) and the CompositionRoot scene: the export parents one of these into
// CompositionRoot's `videoSlot`, exactly where the live preview parents its
// PreviewVideo, so the SAME composition styles both.
//
// THREAD CONTRACT
//   setFrame(QImage) is safe to call from ANY thread. It copies the image under
//   a mutex and requests a repaint: when already on the GUI (item) thread it
//   calls update() directly — which the offscreen export relies on, because it
//   renders synchronously right after pushing a frame — otherwise it posts a
//   queued invoke so update() runs on the GUI thread. NO scene-graph object is
//   ever touched off-thread.
//   updatePaintNode() runs on the render thread (or, under QQuickRenderControl,
//   the render-driving thread) during sync; it is the ONLY place a QSGTexture is
//   created or assigned. The texture is (re)created from the pending image; the
//   node geometry follows the item's bounding rect.
class VideoFrameItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit VideoFrameItem(QQuickItem *parent = nullptr);
    ~VideoFrameItem() override;

    // Push a new frame. Thread-safe (see the contract above). A null/empty image
    // clears the item to transparent.
    void setFrame(const QImage &frame);

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void requestRepaint();

    QMutex m_mutex;
    QImage m_pending;      // guarded by m_mutex; taken in updatePaintNode
    bool m_hasPending = false;
};
