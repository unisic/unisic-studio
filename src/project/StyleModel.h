#pragma once
#include <QColor>
#include <QJsonObject>
#include <QObject>
#include <QString>

// The look of the composed video: background, padding/rounding, drop shadow,
// window frame, aspect, and cursor/click styling. Pure presentation state —
// every field is a Q_PROPERTY so the style panel two-way-binds to it, and every
// setter fires both its own NOTIFY and a coalesced changed() that StudioProject
// uses for dirty-tracking (so it needn't subscribe to two dozen signals).
//
// Defaults below are the app's out-of-the-box look; they double as the values a
// missing JSON key falls back to. Kit palette tokens: Primary #17153B,
// Tertiary #433D8B, Accent #C8ACD6.
class StyleModel : public QObject
{
    Q_OBJECT

    // color | gradient | wallpaper | desktopBlur
    Q_PROPERTY(QString backgroundType READ backgroundType WRITE setBackgroundType NOTIFY backgroundTypeChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY backgroundColorChanged)
    Q_PROPERTY(QColor gradientStart READ gradientStart WRITE setGradientStart NOTIFY gradientStartChanged)
    Q_PROPERTY(QColor gradientEnd READ gradientEnd WRITE setGradientEnd NOTIFY gradientEndChanged)
    Q_PROPERTY(QString wallpaperPath READ wallpaperPath WRITE setWallpaperPath NOTIFY wallpaperPathChanged)
    Q_PROPERTY(double paddingPct READ paddingPct WRITE setPaddingPct NOTIFY paddingPctChanged)
    Q_PROPERTY(int cornerRadius READ cornerRadius WRITE setCornerRadius NOTIFY cornerRadiusChanged)
    Q_PROPERTY(int shadowBlur READ shadowBlur WRITE setShadowBlur NOTIFY shadowBlurChanged)
    Q_PROPERTY(double shadowOpacity READ shadowOpacity WRITE setShadowOpacity NOTIFY shadowOpacityChanged)
    Q_PROPERTY(int shadowOffsetY READ shadowOffsetY WRITE setShadowOffsetY NOTIFY shadowOffsetYChanged)
    // none | minimal | titlebar
    Q_PROPERTY(QString frameStyle READ frameStyle WRITE setFrameStyle NOTIFY frameStyleChanged)
    Q_PROPERTY(QString frameTitle READ frameTitle WRITE setFrameTitle NOTIFY frameTitleChanged)
    // source | 16:9 | 9:16 | 1:1
    Q_PROPERTY(QString aspect READ aspect WRITE setAspect NOTIFY aspectChanged)
    Q_PROPERTY(double cursorScale READ cursorScale WRITE setCursorScale NOTIFY cursorScaleChanged)
    // system | dot | circle
    Q_PROPERTY(QString cursorStyle READ cursorStyle WRITE setCursorStyle NOTIFY cursorStyleChanged)
    Q_PROPERTY(bool clickRipple READ clickRipple WRITE setClickRipple NOTIFY clickRippleChanged)
    Q_PROPERTY(QColor rippleColor READ rippleColor WRITE setRippleColor NOTIFY rippleColorChanged)

public:
    explicit StyleModel(QObject *parent = nullptr);

    QString backgroundType() const { return m_backgroundType; }
    QColor backgroundColor() const { return m_backgroundColor; }
    QColor gradientStart() const { return m_gradientStart; }
    QColor gradientEnd() const { return m_gradientEnd; }
    QString wallpaperPath() const { return m_wallpaperPath; }
    double paddingPct() const { return m_paddingPct; }
    int cornerRadius() const { return m_cornerRadius; }
    int shadowBlur() const { return m_shadowBlur; }
    double shadowOpacity() const { return m_shadowOpacity; }
    int shadowOffsetY() const { return m_shadowOffsetY; }
    QString frameStyle() const { return m_frameStyle; }
    QString frameTitle() const { return m_frameTitle; }
    QString aspect() const { return m_aspect; }
    double cursorScale() const { return m_cursorScale; }
    QString cursorStyle() const { return m_cursorStyle; }
    bool clickRipple() const { return m_clickRipple; }
    QColor rippleColor() const { return m_rippleColor; }

    void setBackgroundType(const QString &v);
    void setBackgroundColor(const QColor &v);
    void setGradientStart(const QColor &v);
    void setGradientEnd(const QColor &v);
    void setWallpaperPath(const QString &v);
    void setPaddingPct(double v);
    void setCornerRadius(int v);
    void setShadowBlur(int v);
    void setShadowOpacity(double v);
    void setShadowOffsetY(int v);
    void setFrameStyle(const QString &v);
    void setFrameTitle(const QString &v);
    void setAspect(const QString &v);
    void setCursorScale(double v);
    void setCursorStyle(const QString &v);
    void setClickRipple(bool v);
    void setRippleColor(const QColor &v);

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &o);

signals:
    void backgroundTypeChanged();
    void backgroundColorChanged();
    void gradientStartChanged();
    void gradientEndChanged();
    void wallpaperPathChanged();
    void paddingPctChanged();
    void cornerRadiusChanged();
    void shadowBlurChanged();
    void shadowOpacityChanged();
    void shadowOffsetYChanged();
    void frameStyleChanged();
    void frameTitleChanged();
    void aspectChanged();
    void cursorScaleChanged();
    void cursorStyleChanged();
    void clickRippleChanged();
    void rippleColorChanged();

    // Fired by every setter after its own NOTIFY — the single hook dirty
    // tracking listens on.
    void changed();

private:
    QString m_backgroundType = QStringLiteral("color");
    QColor m_backgroundColor = QColor(0x17, 0x15, 0x3B);   // Primary
    QColor m_gradientStart = QColor(0x17, 0x15, 0x3B);     // Primary
    QColor m_gradientEnd = QColor(0x43, 0x3D, 0x8B);       // Tertiary
    QString m_wallpaperPath;
    double m_paddingPct = 8.0;      // clamped [0, 30]
    int m_cornerRadius = 12;
    int m_shadowBlur = 48;
    double m_shadowOpacity = 0.35;
    int m_shadowOffsetY = 10;
    QString m_frameStyle = QStringLiteral("none");
    QString m_frameTitle;
    QString m_aspect = QStringLiteral("source");
    double m_cursorScale = 1.0;
    QString m_cursorStyle = QStringLiteral("system");
    bool m_clickRipple = true;
    QColor m_rippleColor = QColor(0xC8, 0xAC, 0xD6);        // Accent
};
