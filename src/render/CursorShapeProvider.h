#pragma once
#include <QList>
#include <QQuickImageProvider>
#include <QString>

struct CursorShape;

// Serves recorded cursor bitmaps to the overlay via image://cursorshape/<id>,
// where <id> is "<projectId>/<shapeId>". The recording captured the pointer in
// Metadata mode (no baked-in cursor), so the overlay draws it — feeding a PNG
// through a real image URL keeps base64 data-URLs out of the per-frame hot path.
//
// The bitmap store is a PROCESS-WIDE, mutex-guarded registry, ref-counted per
// projectId: the same project can be registered by the live preview and by an
// export at once, and the shapes are decoded once and freed when the last holder
// releases. requestImage() may run off the GUI thread (async Image loads), hence
// the mutex. A fresh provider instance is handed to each QQmlEngine (the engine
// takes ownership); they all read the one shared registry.
class CursorShapeProvider : public QQuickImageProvider
{
public:
    CursorShapeProvider();

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

    // Decode + store a project's shapes (idempotent; ref-counted by projectId).
    static void registerShapes(const QString &projectId, const QList<CursorShape> &shapes);
    // Drop one hold on a project's shapes; frees them at zero.
    static void releaseProject(const QString &projectId);

    // "image://cursorshape/<projectId>/<shapeId>" — the source URL the overlay
    // binds. Kept in sync with CursorPlayback's own formatter.
    static QString urlFor(const QString &projectId, int shapeId);
};
