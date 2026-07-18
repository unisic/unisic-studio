#include "ZoomTimeline.h"

#include <QJsonArray>
#include <algorithm>

namespace {

double normalizedParam(const QJsonObject &params, const char *name, double fallback)
{
    const double value = params.value(QLatin1String(name)).toDouble(fallback);
    return qBound(0.0, value, 1.0);
}

} // namespace

ZoomTimeline::ZoomTimeline(QObject *parent)
    : QAbstractListModel(parent)
{
}

int ZoomTimeline::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_keyframes.size();
}

QVariant ZoomTimeline::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_keyframes.size())
        return {};
    const Keyframe &kf = m_keyframes.at(index.row());
    switch (role) {
    case TMsRole:     return double(kf.tMs);
    case RectRole:    return kf.rect;
    case EaseInRole:  return kf.easeInMs;
    case EaseOutRole: return kf.easeOutMs;
    case SourceRole:  return int(kf.source);
    case LockedRole:  return kf.locked;
    default:          return {};
    }
}

QHash<int, QByteArray> ZoomTimeline::roleNames() const
{
    return {
        {TMsRole, "tMs"},
        {RectRole, "rect"},
        {EaseInRole, "easeInMs"},
        {EaseOutRole, "easeOutMs"},
        {SourceRole, "source"},
        {LockedRole, "locked"},
    };
}

QVariantMap ZoomTimeline::keyframeAt(int index) const
{
    if (index < 0 || index >= m_keyframes.size())
        return {};
    const Keyframe &kf = m_keyframes.at(index);
    return QVariantMap{
        {QStringLiteral("tMs"), double(kf.tMs)},
        {QStringLiteral("x"), kf.rect.x()},
        {QStringLiteral("y"), kf.rect.y()},
        {QStringLiteral("w"), kf.rect.width()},
        {QStringLiteral("h"), kf.rect.height()},
        {QStringLiteral("easeInMs"), kf.easeInMs},
        {QStringLiteral("easeOutMs"), kf.easeOutMs},
        {QStringLiteral("source"), int(kf.source)},
        {QStringLiteral("locked"), kf.locked},
    };
}

int ZoomTimeline::insertionRow(qint64 t) const
{
    // upper_bound: land AFTER equal-tMs keyframes so ties keep insertion order.
    const auto it = std::upper_bound(
        m_keyframes.cbegin(), m_keyframes.cend(), t,
        [](qint64 tt, const Keyframe &kf) { return tt < kf.tMs; });
    return int(it - m_keyframes.cbegin());
}

int ZoomTimeline::addKeyframe(const Keyframe &kf)
{
    const int row = insertionRow(kf.tMs);
    beginInsertRows({}, row, row);
    m_keyframes.insert(row, kf);
    endInsertRows();
    emit countChanged();
    emit changed();
    return row;
}

void ZoomTimeline::addKeyframes(const QList<Keyframe> &kfs)
{
    if (kfs.isEmpty())
        return;
    beginResetModel();
    m_keyframes.append(kfs);
    // The backing list must stay sorted by tMs. stable_sort keeps equal-tMs rows in
    // their prior relative order — existing keyframes ahead of the freshly appended
    // ones — so ties land exactly where a per-item addKeyframe() (upper_bound
    // insert) would have put them.
    std::stable_sort(m_keyframes.begin(), m_keyframes.end(),
                     [](const Keyframe &a, const Keyframe &b) { return a.tMs < b.tMs; });
    endResetModel();
    emit countChanged();
    emit changed();
}

void ZoomTimeline::replaceAutoKeyframes(const QList<Keyframe> &keyframes)
{
    QList<Keyframe> next;
    next.reserve(m_keyframes.size() + keyframes.size());
    for (const Keyframe &keyframe : std::as_const(m_keyframes)) {
        if (keyframe.source == Auto && !keyframe.locked)
            continue;
        next.append(keyframe);
    }
    next.append(keyframes);
    std::stable_sort(next.begin(), next.end(),
                     [](const Keyframe &a, const Keyframe &b) { return a.tMs < b.tMs; });

    const bool countChangedValue = next.size() != m_keyframes.size();
    beginResetModel();
    m_keyframes = std::move(next);
    endResetModel();
    if (countChangedValue)
        emit countChanged();
    emit changed();
}

void ZoomTimeline::removeAt(int index)
{
    if (index < 0 || index >= m_keyframes.size())
        return;
    beginRemoveRows({}, index, index);
    m_keyframes.removeAt(index);
    endRemoveRows();
    emit countChanged();
    emit changed();
}

// A hand edit on an unlocked Auto keyframe locks it in place — otherwise the
// next implicit regeneration (intensity slider, aspect switch, …) silently
// discards the user's work. Locked Auto keyframes survive replaceAutoKeyframes/
// clearAuto and the generator routes its spans around them (pinned windows).
void ZoomTimeline::pinOnUserEdit(int index)
{
    Keyframe &kf = m_keyframes[index];
    if (kf.source != Auto || kf.locked)
        return;
    kf.locked = true;
    emit dataChanged(this->index(index), this->index(index), {LockedRole});
}

int ZoomTimeline::moveKeyframe(int index, qint64 newT)
{
    if (index < 0 || index >= m_keyframes.size())
        return -1;

    pinOnUserEdit(index);
    Keyframe kf = m_keyframes.at(index);
    kf.tMs = newT;

    // Remove then sorted-reinsert. Compute the destination against the list as
    // it looks WITHOUT the moved row so the returned index is the real landing
    // spot (removing a lower row shifts the target down by one).
    beginRemoveRows({}, index, index);
    m_keyframes.removeAt(index);
    endRemoveRows();

    const int row = insertionRow(newT);
    beginInsertRows({}, row, row);
    m_keyframes.insert(row, kf);
    endInsertRows();

    emit changed();
    return row;
}

void ZoomTimeline::setKeyframeRect(int index, const QRectF &rect)
{
    if (index < 0 || index >= m_keyframes.size())
        return;
    pinOnUserEdit(index);
    m_keyframes[index].rect = rect;
    emit dataChanged(this->index(index), this->index(index), {RectRole});
    emit changed();
}

void ZoomTimeline::setKeyframeLocked(int index, bool locked)
{
    if (index < 0 || index >= m_keyframes.size())
        return;
    m_keyframes[index].locked = locked;
    emit dataChanged(this->index(index), this->index(index), {LockedRole});
    emit changed();
}

void ZoomTimeline::setKeyframeEasing(int index, int easeInMs, int easeOutMs)
{
    if (index < 0 || index >= m_keyframes.size())
        return;
    pinOnUserEdit(index);
    m_keyframes[index].easeInMs = easeInMs;
    m_keyframes[index].easeOutMs = easeOutMs;
    emit dataChanged(this->index(index), this->index(index), {EaseInRole, EaseOutRole});
    emit changed();
}

void ZoomTimeline::clearAuto()
{
    // Rebuild the survivors; a single reset is cheaper and simpler than
    // per-row removals when a regen can wipe most of the list.
    QList<Keyframe> kept;
    kept.reserve(m_keyframes.size());
    for (const Keyframe &kf : std::as_const(m_keyframes)) {
        if (kf.source == Auto && !kf.locked)
            continue;   // an unlocked auto keyframe — the regen owns it
        kept.append(kf);
    }
    if (kept.size() == m_keyframes.size())
        return;         // nothing to drop
    beginResetModel();
    m_keyframes = std::move(kept);
    endResetModel();
    emit countChanged();
    emit changed();
}

void ZoomTimeline::clear()
{
    const bool hadFrames = !m_keyframes.isEmpty();
    const bool hadParams = !m_autoParams.isEmpty();
    if (!hadFrames && !hadParams)
        return;

    const double oldIntensity = zoomIntensity();
    const double oldSmoothness = motionSmoothness();
    if (hadFrames) {
        beginResetModel();
        m_keyframes.clear();
        endResetModel();
        emit countChanged();
    }
    m_autoParams = {};
    if (!qFuzzyCompare(oldIntensity, zoomIntensity()))
        emit zoomIntensityChanged();
    if (!qFuzzyCompare(oldSmoothness, motionSmoothness()))
        emit motionSmoothnessChanged();
    emit changed();
}

double ZoomTimeline::zoomIntensity() const
{
    return normalizedParam(m_autoParams, "zoomIntensity", DefaultZoomIntensity);
}

double ZoomTimeline::motionSmoothness() const
{
    return normalizedParam(m_autoParams, "motionSmoothness", DefaultMotionSmoothness);
}

void ZoomTimeline::setZoomIntensity(double value)
{
    value = qBound(0.0, value, 1.0);
    if (qFuzzyCompare(zoomIntensity(), value))
        return;
    m_autoParams.insert(QStringLiteral("zoomIntensity"), value);
    emit zoomIntensityChanged();
    emit changed();
}

void ZoomTimeline::setMotionSmoothness(double value)
{
    value = qBound(0.0, value, 1.0);
    if (qFuzzyCompare(motionSmoothness(), value))
        return;
    m_autoParams.insert(QStringLiteral("motionSmoothness"), value);
    emit motionSmoothnessChanged();
    emit changed();
}

void ZoomTimeline::setAutoParams(const QJsonObject &params)
{
    if (m_autoParams == params)
        return;
    const double oldIntensity = zoomIntensity();
    const double oldSmoothness = motionSmoothness();
    m_autoParams = params;
    if (!qFuzzyCompare(oldIntensity, zoomIntensity()))
        emit zoomIntensityChanged();
    if (!qFuzzyCompare(oldSmoothness, motionSmoothness()))
        emit motionSmoothnessChanged();
    emit changed();
}

QJsonObject ZoomTimeline::toJson() const
{
    QJsonArray arr;
    for (const Keyframe &kf : m_keyframes) {
        arr.append(QJsonObject{
            {QStringLiteral("t"), double(kf.tMs)},
            {QStringLiteral("x"), kf.rect.x()},
            {QStringLiteral("y"), kf.rect.y()},
            {QStringLiteral("w"), kf.rect.width()},
            {QStringLiteral("h"), kf.rect.height()},
            {QStringLiteral("easeIn"), kf.easeInMs},
            {QStringLiteral("easeOut"), kf.easeOutMs},
            {QStringLiteral("source"), int(kf.source)},
            {QStringLiteral("locked"), kf.locked},
        });
    }
    return QJsonObject{
        {QStringLiteral("keyframes"), arr},
        {QStringLiteral("autoParams"), m_autoParams},
    };
}

void ZoomTimeline::fromJson(const QJsonObject &o)
{
    const double oldIntensity = zoomIntensity();
    const double oldSmoothness = motionSmoothness();
    beginResetModel();
    m_keyframes.clear();
    for (const QJsonValue &v : o.value(QStringLiteral("keyframes")).toArray()) {
        const QJsonObject k = v.toObject();
        Keyframe kf;
        kf.tMs = qint64(k.value(QStringLiteral("t")).toDouble());
        kf.rect = QRectF(k.value(QStringLiteral("x")).toDouble(),
                         k.value(QStringLiteral("y")).toDouble(),
                         k.value(QStringLiteral("w")).toDouble(),
                         k.value(QStringLiteral("h")).toDouble());
        kf.easeInMs = k.value(QStringLiteral("easeIn")).toInt();
        kf.easeOutMs = k.value(QStringLiteral("easeOut")).toInt();
        kf.source = k.value(QStringLiteral("source")).toInt() == int(Auto) ? Auto : Manual;
        kf.locked = k.value(QStringLiteral("locked")).toBool();
        m_keyframes.append(kf);
    }
    // Defensive: a hand-edited file might be out of order; the whole model
    // contract assumes sorted rows.
    std::stable_sort(m_keyframes.begin(), m_keyframes.end(),
                     [](const Keyframe &a, const Keyframe &b) { return a.tMs < b.tMs; });
    m_autoParams = o.value(QStringLiteral("autoParams")).toObject();
    endResetModel();
    emit countChanged();
    if (!qFuzzyCompare(oldIntensity, zoomIntensity()))
        emit zoomIntensityChanged();
    if (!qFuzzyCompare(oldSmoothness, motionSmoothness()))
        emit motionSmoothnessChanged();
    emit changed();
}
