#include "StyleModel.h"

StyleModel::StyleModel(QObject *parent)
    : QObject(parent)
{
}

// Each setter: no-op if unchanged (keeps dirty-tracking and binding loops
// honest), else store, emit the property's own NOTIFY, then the coalesced
// changed(). Colours serialize as #AARRGGBB so alpha survives the roundtrip.

void StyleModel::setBackgroundType(const QString &v)
{
    if (m_backgroundType == v) return;
    m_backgroundType = v;
    emit backgroundTypeChanged();
    emit changed();
}

void StyleModel::setBackgroundColor(const QColor &v)
{
    if (m_backgroundColor == v) return;
    m_backgroundColor = v;
    emit backgroundColorChanged();
    emit changed();
}

void StyleModel::setGradientStart(const QColor &v)
{
    if (m_gradientStart == v) return;
    m_gradientStart = v;
    emit gradientStartChanged();
    emit changed();
}

void StyleModel::setGradientEnd(const QColor &v)
{
    if (m_gradientEnd == v) return;
    m_gradientEnd = v;
    emit gradientEndChanged();
    emit changed();
}

void StyleModel::setWallpaperPath(const QString &v)
{
    if (m_wallpaperPath == v) return;
    m_wallpaperPath = v;
    emit wallpaperPathChanged();
    emit changed();
}

void StyleModel::setPaddingPct(double v)
{
    v = qBound(0.0, v, 30.0);   // the panel slider's range; guard bad input too
    if (qFuzzyCompare(m_paddingPct, v)) return;
    m_paddingPct = v;
    emit paddingPctChanged();
    emit changed();
}

void StyleModel::setCornerRadius(int v)
{
    if (m_cornerRadius == v) return;
    m_cornerRadius = v;
    emit cornerRadiusChanged();
    emit changed();
}

void StyleModel::setShadowBlur(int v)
{
    if (m_shadowBlur == v) return;
    m_shadowBlur = v;
    emit shadowBlurChanged();
    emit changed();
}

void StyleModel::setShadowOpacity(double v)
{
    v = qBound(0.0, v, 1.0);
    if (qFuzzyCompare(m_shadowOpacity, v)) return;
    m_shadowOpacity = v;
    emit shadowOpacityChanged();
    emit changed();
}

void StyleModel::setShadowOffsetY(int v)
{
    if (m_shadowOffsetY == v) return;
    m_shadowOffsetY = v;
    emit shadowOffsetYChanged();
    emit changed();
}

void StyleModel::setFrameStyle(const QString &v)
{
    if (m_frameStyle == v) return;
    m_frameStyle = v;
    emit frameStyleChanged();
    emit changed();
}

void StyleModel::setFrameTitle(const QString &v)
{
    if (m_frameTitle == v) return;
    m_frameTitle = v;
    emit frameTitleChanged();
    emit changed();
}

void StyleModel::setAspect(const QString &v)
{
    if (m_aspect == v) return;
    m_aspect = v;
    emit aspectChanged();
    emit changed();
}

void StyleModel::setFillMode(const QString &v)
{
    if (m_fillMode == v) return;
    m_fillMode = v;
    emit fillModeChanged();
    emit changed();
}

void StyleModel::setCursorScale(double v)
{
    if (qFuzzyCompare(m_cursorScale, v)) return;
    m_cursorScale = v;
    emit cursorScaleChanged();
    emit changed();
}

void StyleModel::setCursorStyle(const QString &v)
{
    if (m_cursorStyle == v) return;
    m_cursorStyle = v;
    emit cursorStyleChanged();
    emit changed();
}

void StyleModel::setClickRipple(bool v)
{
    if (m_clickRipple == v) return;
    m_clickRipple = v;
    emit clickRippleChanged();
    emit changed();
}

void StyleModel::setRippleColor(const QColor &v)
{
    if (m_rippleColor == v) return;
    m_rippleColor = v;
    emit rippleColorChanged();
    emit changed();
}

void StyleModel::setWebcamEnabled(bool v)
{
    if (m_webcamEnabled == v) return;
    m_webcamEnabled = v;
    emit webcamEnabledChanged();
    emit changed();
}

void StyleModel::setWebcamPosition(const QString &v)
{
    if (m_webcamPosition == v) return;
    m_webcamPosition = v;
    emit webcamPositionChanged();
    emit changed();
}

void StyleModel::setWebcamSizePct(double v)
{
    v = qBound(8.0, v, 40.0);
    if (qFuzzyCompare(m_webcamSizePct, v)) return;
    m_webcamSizePct = v;
    emit webcamSizePctChanged();
    emit changed();
}

void StyleModel::setWebcamRounded(bool v)
{
    if (m_webcamRounded == v) return;
    m_webcamRounded = v;
    emit webcamRoundedChanged();
    emit changed();
}

QJsonObject StyleModel::toJson() const
{
    return QJsonObject{
        {QStringLiteral("backgroundType"), m_backgroundType},
        {QStringLiteral("backgroundColor"), m_backgroundColor.name(QColor::HexArgb)},
        {QStringLiteral("gradientStart"), m_gradientStart.name(QColor::HexArgb)},
        {QStringLiteral("gradientEnd"), m_gradientEnd.name(QColor::HexArgb)},
        {QStringLiteral("wallpaperPath"), m_wallpaperPath},
        {QStringLiteral("paddingPct"), m_paddingPct},
        {QStringLiteral("cornerRadius"), m_cornerRadius},
        {QStringLiteral("shadowBlur"), m_shadowBlur},
        {QStringLiteral("shadowOpacity"), m_shadowOpacity},
        {QStringLiteral("shadowOffsetY"), m_shadowOffsetY},
        {QStringLiteral("frameStyle"), m_frameStyle},
        {QStringLiteral("frameTitle"), m_frameTitle},
        {QStringLiteral("aspect"), m_aspect},
        {QStringLiteral("fillMode"), m_fillMode},
        {QStringLiteral("cursorScale"), m_cursorScale},
        {QStringLiteral("cursorStyle"), m_cursorStyle},
        {QStringLiteral("clickRipple"), m_clickRipple},
        {QStringLiteral("rippleColor"), m_rippleColor.name(QColor::HexArgb)},
        {QStringLiteral("webcamEnabled"), m_webcamEnabled},
        {QStringLiteral("webcamPosition"), m_webcamPosition},
        {QStringLiteral("webcamSizePct"), m_webcamSizePct},
        {QStringLiteral("webcamRounded"), m_webcamRounded},
    };
}

void StyleModel::fromJson(const QJsonObject &o)
{
    // Setters (not raw assignment) so a load fixes up clamps and emits the
    // NOTIFY/changed() the UI and dirty-tracking expect. Missing keys keep the
    // current value (which is the default for a fresh model).
    const auto color = [](const QJsonValue &v, const QColor &fallback) {
        if (!v.isString()) return fallback;
        const QColor c(v.toString());
        return c.isValid() ? c : fallback;
    };
    setBackgroundType(o.value(QStringLiteral("backgroundType")).toString(m_backgroundType));
    setBackgroundColor(color(o.value(QStringLiteral("backgroundColor")), m_backgroundColor));
    setGradientStart(color(o.value(QStringLiteral("gradientStart")), m_gradientStart));
    setGradientEnd(color(o.value(QStringLiteral("gradientEnd")), m_gradientEnd));
    setWallpaperPath(o.value(QStringLiteral("wallpaperPath")).toString(m_wallpaperPath));
    setPaddingPct(o.value(QStringLiteral("paddingPct")).toDouble(m_paddingPct));
    setCornerRadius(o.value(QStringLiteral("cornerRadius")).toInt(m_cornerRadius));
    setShadowBlur(o.value(QStringLiteral("shadowBlur")).toInt(m_shadowBlur));
    setShadowOpacity(o.value(QStringLiteral("shadowOpacity")).toDouble(m_shadowOpacity));
    setShadowOffsetY(o.value(QStringLiteral("shadowOffsetY")).toInt(m_shadowOffsetY));
    setFrameStyle(o.value(QStringLiteral("frameStyle")).toString(m_frameStyle));
    setFrameTitle(o.value(QStringLiteral("frameTitle")).toString(m_frameTitle));
    setAspect(o.value(QStringLiteral("aspect")).toString(m_aspect));
    setFillMode(o.value(QStringLiteral("fillMode")).toString(m_fillMode));
    setCursorScale(o.value(QStringLiteral("cursorScale")).toDouble(m_cursorScale));
    setCursorStyle(o.value(QStringLiteral("cursorStyle")).toString(m_cursorStyle));
    setClickRipple(o.value(QStringLiteral("clickRipple")).toBool(m_clickRipple));
    setRippleColor(color(o.value(QStringLiteral("rippleColor")), m_rippleColor));
    setWebcamEnabled(o.value(QStringLiteral("webcamEnabled")).toBool(m_webcamEnabled));
    setWebcamPosition(o.value(QStringLiteral("webcamPosition")).toString(m_webcamPosition));
    setWebcamSizePct(o.value(QStringLiteral("webcamSizePct")).toDouble(m_webcamSizePct));
    setWebcamRounded(o.value(QStringLiteral("webcamRounded")).toBool(m_webcamRounded));
}
