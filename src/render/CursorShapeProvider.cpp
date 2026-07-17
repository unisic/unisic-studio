#include "CursorShapeProvider.h"

#include "project/CursorTrack.h"

#include <QHash>
#include <QMutex>

namespace {
// One process-wide store. Guarded by s_mutex; ref-counted per projectId so
// overlapping holders (preview + export) share one decode and free once.
QMutex s_mutex;
QHash<QString, QImage> s_images;   // key "<projectId>/<shapeId>" -> decoded bitmap
QHash<QString, int> s_refs;        // projectId -> hold count
} // namespace

CursorShapeProvider::CursorShapeProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage CursorShapeProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    QImage img;
    {
        QMutexLocker lock(&s_mutex);
        img = s_images.value(id);
    }
    if (img.isNull())
        return img;
    if (size)
        *size = img.size();
    if (requestedSize.isValid() && requestedSize != img.size())
        return img.scaled(requestedSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return img;
}

void CursorShapeProvider::registerShapes(const QString &projectId, const QList<CursorShape> &shapes)
{
    QMutexLocker lock(&s_mutex);
    if (++s_refs[projectId] > 1)
        return; // already decoded by another holder
    for (const CursorShape &sh : shapes) {
        QImage img;
        if (img.loadFromData(sh.png, "PNG") && !img.isNull())
            s_images.insert(QStringLiteral("%1/%2").arg(projectId).arg(sh.id), img);
    }
}

void CursorShapeProvider::releaseProject(const QString &projectId)
{
    QMutexLocker lock(&s_mutex);
    const auto it = s_refs.find(projectId);
    if (it == s_refs.end())
        return;
    if (--it.value() > 0)
        return;
    s_refs.erase(it);
    const QString prefix = projectId + QLatin1Char('/');
    for (auto i = s_images.begin(); i != s_images.end();) {
        if (i.key().startsWith(prefix))
            i = s_images.erase(i);
        else
            ++i;
    }
}

QString CursorShapeProvider::urlFor(const QString &projectId, int shapeId)
{
    return QStringLiteral("image://cursorshape/%1/%2").arg(projectId).arg(shapeId);
}
