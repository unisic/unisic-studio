import QtQuick
import QtQuick.Effects

// THE composition scene: styling + framing of the recorded video. This single
// implementation is shared by the live editor preview AND (next milestone) the
// offscreen QQuickRenderControl export renderer, so it is RIGOROUSLY
// self-contained: every input arrives through a property (a StyleModel, a source
// videoSize, a timeMs, a normalized zoomRect) and the consumer parents the
// actual video Item into `videoSlot`. NO Window references, NO app singletons,
// NO Theme import — an offscreen render context with no app can instantiate it.
//
// All user-controllable colors flow from styleModel; only NEUTRAL chrome inside
// the frame styles (macOS dots, hairline border, title-bar strip) is hardcoded,
// which the design brief explicitly allows.
Item {
    id: root

    // ---- Inputs (the ONLY inputs) ------------------------------------------
    property var  styleModel: null                 // C++ StyleModel*
    property size videoSize: Qt.size(1920, 1080)   // source pixel size
    property real timeMs: 0                         // current playback/render instant
    // Normalized camera viewport in [0,1]; default = whole frame (identity). The
    // zoom/pan engine drives this (preview: PreviewController.zoomRect; export:
    // per-frame from KeyframeEngine::evaluate).
    property rect zoomRect: Qt.rect(0, 0, 1, 1)
    // CursorPlayback*: normalised pointer position, current shape bitmap, active
    // click ripples. Null-safe (the overlay hides itself). Set by the consumer
    // (preview: preview.cursor; export: the RenderPipeline).
    property var cursorPlayback: null
    // A still (file:// URL) of the video's first frame, used ONLY by the
    // "desktopBlur" background. Filled by the consumer (preview: the editor's
    // PosterExtractor; export: RenderPipeline's one-shot extract). Empty → the
    // desktopBlur background falls back to the flat fill colour.
    property string posterSource: ""

    // The consumer parents its video (or poster) Item here and anchors.fill it.
    readonly property alias videoSlot: videoZoom

    // ---- Style accessors (safe fallbacks while styleModel is null) ----------
    readonly property string _bgType:        styleModel ? styleModel.backgroundType : "color"
    readonly property color  _bgColor:       styleModel ? styleModel.backgroundColor : "#17153B"
    readonly property color  _gradA:         styleModel ? styleModel.gradientStart : "#17153B"
    readonly property color  _gradB:         styleModel ? styleModel.gradientEnd : "#433D8B"
    readonly property string _wallpaper:     styleModel ? styleModel.wallpaperPath : ""
    readonly property real   _paddingPct:    styleModel ? styleModel.paddingPct : 8
    readonly property int    _corner:        styleModel ? styleModel.cornerRadius : 12
    readonly property int    _shadowBlur:    styleModel ? styleModel.shadowBlur : 48
    readonly property real   _shadowOpacity: styleModel ? styleModel.shadowOpacity : 0.35
    readonly property int    _shadowOffsetY: styleModel ? styleModel.shadowOffsetY : 10
    readonly property string _frame:         styleModel ? styleModel.frameStyle : "none"
    readonly property string _frameTitle:    styleModel ? styleModel.frameTitle : ""
    readonly property bool   _hasPoster:     root.posterSource !== ""

    // ---- Output geometry ----------------------------------------------------
    readonly property real _srcAspect: videoSize.height > 0
                                       ? videoSize.width / videoSize.height : (16 / 9)
    readonly property real _aspect: {
        switch (styleModel ? styleModel.aspect : "source") {
        case "16:9": return 16 / 9
        case "9:16": return 9 / 16
        case "1:1":  return 1
        default:     return _srcAspect          // "source"
        }
    }
    // Composition canvas: the styled frame, aspect-fit (contain) inside root.
    readonly property real canvasW: (root.width / Math.max(1, root.height)) > _aspect
                                    ? root.height * _aspect : root.width
    readonly property real canvasH: (root.width / Math.max(1, root.height)) > _aspect
                                    ? root.height : root.width / _aspect
    // Resolution-independent scale: radius/shadow are authored against the source
    // video's pixels, so multiply by canvas/source. This keeps the small preview
    // and the full-res export visually identical.
    readonly property real _scale: canvasW / Math.max(1, videoSize.width)
    readonly property real dispRadius: Math.max(0, _corner * _scale)
    // Symmetric padding as a fraction of the canvas' short side.
    readonly property real padPx: Math.round(Math.min(canvasW, canvasH) * _paddingPct / 100)
    readonly property real innerW: Math.max(1, canvasW - 2 * padPx)
    readonly property real innerH: Math.max(1, canvasH - 2 * padPx)
    // Title-bar strip height (0 unless the titlebar frame is chosen).
    readonly property real barH: _frame === "titlebar"
                                 ? Math.round(Math.min(Math.max(innerW * 0.045, 22), 46)) : 0
    // The video is aspect-fit (source ratio) within the area left after the bar.
    readonly property real _availH: Math.max(1, innerH - barH)
    readonly property real videoDispW: (innerW / _availH) > _srcAspect ? _availH * _srcAspect : innerW
    readonly property real videoDispH: (innerW / _availH) > _srcAspect ? _availH : innerW / _srcAspect
    // The framed window = title bar + video region.
    readonly property real holderW: videoDispW
    readonly property real holderH: videoDispH + barH

    // =========================================================================
    Item {
        id: canvas
        width: root.canvasW
        height: root.canvasH
        anchors.centerIn: parent

        // ---- Background ----
        Rectangle {                                     // color / desktopBlur(→color fallback)
            anchors.fill: parent
            visible: root._bgType === "color"
                     || (root._bgType === "desktopBlur" && !root._hasPoster)
            color: root._bgColor
        }
        Rectangle {                                     // gradient (vertical)
            anchors.fill: parent
            visible: root._bgType === "gradient"
            gradient: Gradient {
                GradientStop { position: 0.0; color: root._gradA }
                GradientStop { position: 1.0; color: root._gradB }
            }
        }
        Image {                                         // wallpaper (cover-crop)
            anchors.fill: parent
            visible: root._bgType === "wallpaper" && root._wallpaper !== ""
            source: root._wallpaper === "" ? ""
                    : (/^(file:|qrc:|:|image:)/.test(root._wallpaper)
                       ? root._wallpaper : "file://" + root._wallpaper)
            fillMode: Image.PreserveAspectCrop
            clip: true
            asynchronous: true
            cache: true
        }

        // desktopBlur: a blurred + darkened cover-crop of the video's own first
        // frame. HONEST APPROXIMATION — a true "desktop blur" would blur the
        // user's actual wallpaper, which we never captured; the poster is the
        // cheapest always-available stand-in. The poster is STATIC, so the layer
        // caches to a texture and the blur shader runs ONCE, never per video
        // frame (same discipline as the shadow/mask plates below).
        Item {
            anchors.fill: parent
            visible: root._bgType === "desktopBlur" && root._hasPoster
            Image {
                id: blurPoster
                anchors.fill: parent
                source: root.posterSource
                fillMode: Image.PreserveAspectCrop
                clip: true
                asynchronous: true
                cache: true
                layer.enabled: true
                layer.effect: MultiEffect {
                    blurEnabled: true
                    blur: 1.0
                    blurMax: 64
                    saturation: -0.15
                }
            }
            // Darkening scrim so the framed video card stays the focal point.
            Rectangle { anchors.fill: parent; color: "#000000"; opacity: 0.45 }
        }

        // ---- Drop shadow (cached: static geometry → the costly blur computes
        //       ONCE, not per video frame). The opaque video card covers this
        //       plate exactly, so only its shadow (extending beyond) is seen. ----
        Rectangle {
            id: shadowPlate
            width: root.holderW
            height: root.holderH
            anchors.centerIn: parent
            radius: root.dispRadius
            color: "#000000"
            visible: root._shadowOpacity > 0 && root._shadowBlur > 0
            layer.enabled: visible
            layer.effect: MultiEffect {
                shadowEnabled: true
                blurMax: 64
                shadowBlur: Math.min(1.0, root._shadowBlur * root._scale / 64)
                shadowColor: "#000000"
                shadowOpacity: root._shadowOpacity
                shadowVerticalOffset: root._shadowOffsetY * root._scale
                shadowHorizontalOffset: 0
            }
        }

        // ---- The framed window: one rounded-masked card holding the bar + the
        //       video. A single uniform-radius mask rounds the outer corners; the
        //       bar→video seam is a straight internal line (the macOS look). ----
        Item {
            id: videoCard
            width: root.holderW
            height: root.holderH
            anchors.centerIn: parent

            // Rounded corners via a STATIC mask texture (roundedMask below). The
            // per-frame cost is only a cheap alpha-mask shader over the video; the
            // mask geometry itself never regenerates as the video plays.
            layer.enabled: true
            layer.effect: MultiEffect {
                maskEnabled: true
                maskSource: roundedMask
                maskThresholdMin: 0.5
                maskSpreadAtMin: 1.0
            }

            // Title bar (neutral chrome; only frameTitle is user data).
            Rectangle {
                id: titleBar
                visible: root._frame === "titlebar"
                height: root.barH
                anchors { left: parent.left; right: parent.right; top: parent.top }
                color: "#1e1e24"
                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: root.barH * 0.5
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: root.barH * 0.28
                    Repeater {
                        model: ["#ff5f57", "#febc2e", "#28c840"]
                        Rectangle {
                            width: root.barH * 0.26; height: width; radius: width / 2
                            color: modelData
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }
                Text {
                    anchors.centerIn: parent
                    width: parent.width * 0.5
                    text: root._frameTitle
                    color: "#c8c8d0"
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: Math.max(9, root.barH * 0.4)
                }
            }

            // Fixed clip region for the video (stays put so the camera crop is
            // correct even when the inner content is scaled).
            Item {
                id: videoRegion
                anchors {
                    left: parent.left; right: parent.right; bottom: parent.bottom
                    top: titleBar.visible ? titleBar.bottom : parent.top
                }
                clip: true

                // Zoom/pan camera. Map the normalized zoomRect region of the
                // video to fill videoRegion: show [zx..zx+zw]×[zy..zy+zh] scaled
                // to (W,H) with sx=1/zw, sy=1/zh and pan (-zx*W*sx, -zy*H*sy).
                // Default zoomRect (0,0,1,1) → identity. The transform is on this
                // INNER item (not videoRegion) so videoRegion's clip rect stays
                // fixed — the crop is exact. The keyframe engine sets zoomRect.
                Item {
                    id: videoZoom
                    anchors.fill: parent
                    transformOrigin: Item.TopLeft
                    transform: [
                        Scale {
                            xScale: 1 / Math.max(0.0001, root.zoomRect.width)
                            yScale: 1 / Math.max(0.0001, root.zoomRect.height)
                        },
                        Translate {
                            x: -root.zoomRect.x * videoZoom.width / Math.max(0.0001, root.zoomRect.width)
                            y: -root.zoomRect.y * videoZoom.height / Math.max(0.0001, root.zoomRect.height)
                        }
                    ]

                    // Cursor + click ripples, above the (runtime-parented) video and
                    // inside the same zoom transform. z keeps it over that video Item.
                    CursorOverlay {
                        anchors.fill: parent
                        z: 50
                        cursorData: root.cursorPlayback
                        styleModel: root.styleModel
                        videoSize: root.videoSize
                    }
                }
            }
        }

        // Minimal frame: a hairline neutral border over the card (not masked).
        Rectangle {
            visible: root._frame === "minimal"
            width: root.holderW
            height: root.holderH
            anchors.centerIn: parent
            radius: root.dispRadius
            color: "transparent"
            border.width: Math.max(1, Math.round(root._scale))
            border.color: Qt.rgba(1, 1, 1, 0.14)
        }

        // The rounded mask SOURCE — static geometry, rendered to a texture once.
        Item {
            id: roundedMask
            width: root.holderW
            height: root.holderH
            anchors.centerIn: parent
            visible: false
            layer.enabled: true
            Rectangle { anchors.fill: parent; radius: root.dispRadius; color: "black" }
        }
    }
}
