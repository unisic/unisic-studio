import QtQuick
import QtQuick.Effects

// Cursor and click feedback shared byte-for-byte by preview and export. Position,
// idle opacity and press response come from CursorPlayback, so all motion is a
// deterministic function of project time. Art remains inside the camera transform
// for correct anchoring, but local geometry compensates that transform to keep a
// readable, nearly constant on-screen size at every zoom level.
Item {
    id: overlay

    property var cursorData: null
    property var styleModel: null
    property size videoSize: Qt.size(1920, 1080)
    property rect cameraRect: Qt.rect(0, 0, 1, 1)
    property real cameraZoom: 1

    readonly property real timeMs: cursorData ? cursorData.timeMs : 0
    readonly property real _cursorScale: styleModel ? styleModel.cursorScale : 1
    readonly property string _style: styleModel ? styleModel.cursorStyle : "pointer"
    readonly property color _rippleColor: styleModel ? styleModel.rippleColor : "#C8ACD6"
    readonly property bool _rippleOn: styleModel ? styleModel.clickRipple : true
    readonly property real _cursorX: cursorData ? cursorData.nx * width : 0
    readonly property real _cursorY: cursorData ? cursorData.ny * height : 0
    readonly property bool _cursorVisible: cursorData ? cursorData.cursorVisible : false
    readonly property real _cursorOpacity: cursorData ? cursorData.cursorOpacity : 0
    readonly property real _pressScale: (_rippleOn && cursorData) ? cursorData.pressScale : 1

    // Full compensation would keep size exactly constant. Retaining 8% of camera
    // zoom gives subtle emphasis without producing a giant pointer at 2.8x.
    readonly property real _zoomGrowth: Math.pow(Math.max(1, cameraZoom), 0.08)
    readonly property real _camW: Math.max(0.0001, cameraRect.width)
    readonly property real _camH: Math.max(0.0001, cameraRect.height)
    // Proportional to composition height: preview and export remain visually
    // identical when one is scaled to the other, independent of source resolution.
    readonly property real _screenH: height * 0.028 * _cursorScale * _zoomGrowth

    // Premium pointer SVG: 24x32, hotspot (2,1.5). Width/height are authored in
    // pre-camera local units; after parent scaling they resolve to _screenH.
    readonly property real _ptrScreenW: _screenH * (24 / 32)
    readonly property real _ptrW: _ptrScreenW * _pressScale
    readonly property real _ptrH: _screenH * _pressScale
    readonly property real _ptrHotX: _ptrW * (2 / 24)
    readonly property real _ptrHotY: _ptrH * (1.5 / 32)
    readonly property bool _haveSystemShape: cursorData && cursorData.shapeUrl !== ""

    // Supersample factor for the pointer's raster + its shadow layer FBO. The
    // built-in pointer is captured into a MultiEffect layer for the drop shadow;
    // a QtQuick layer's FBO is sized to the item's LOCAL geometry (_ptrW x _ptrH),
    // so without this the crisp SVG is flattened to ~cursor-size texels and the
    // camera/display magnification then upsamples it — the "pixelated cursor on
    // aspect change" bug. A fixed 3x is deliberate: the pointer is camera-
    // compensated to a near-constant on-screen size, so its FBO stays small and
    // constant (no per-frame realloc), and 3x covers device-pixel-ratio and the
    // editor's fit-to-viewport upscale with headroom to spare.
    readonly property real _ptrSuper: 3

    // Supersample for the RECORDED bitmap cursor ("system" style). The shape is a
    // small compositor capture (typically 24–32 px), so once cursorScale/zoom
    // enlarge it there is no more detail — but a single high-quality resample to
    // this many source texels (vs the old fixed 2x, which upscaled to 64 px and
    // then let QML resample AGAIN) keeps the enlarged bitmap as smooth as its
    // native resolution allows. Constant, so the texture is not reallocated per
    // frame. For a truly crisp pointer at any zoom, use the vector "pointer" style.
    readonly property real _sysSuper: 4

    // Soft expanding highlight + clean ring. Geometry is camera-compensated, so
    // both remain circular and equally weighted in the final frame.
    Repeater {
        model: overlay.cursorData ? overlay.cursorData.ripples : null
        delegate: Item {
            required property real nx
            required property real ny
            required property real tMs
            readonly property real phase: {
                if (!overlay.cursorData) return 1
                var p = (overlay.timeMs - tMs) / overlay.cursorData.rippleMs
                return Math.max(0, Math.min(1, p))
            }
            readonly property real eased: 1 - Math.pow(1 - phase, 3)
            readonly property real screenD: Math.min(overlay.width, overlay.height)
                                                * (0.018 + 0.075 * eased)
            visible: overlay._rippleOn && phase < 1

            Item {
                id: rippleVisual
                x: parent.nx * overlay.width - width / 2
                y: parent.ny * overlay.height - height / 2
                width: parent.screenD
                height: parent.screenD
                transformOrigin: Item.Center
                transform: Scale {
                    origin.x: rippleVisual.width / 2
                    origin.y: rippleVisual.height / 2
                    xScale: overlay._camW
                    yScale: overlay._camH
                }
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: overlay._rippleColor
                    opacity: 0.10 * Math.pow(1 - parent.parent.phase, 2)
                }
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: "transparent"
                    border.color: overlay._rippleColor
                    border.width: Math.max(0.7,
                                           Math.min(overlay.width, overlay.height) * 0.0028
                                           * (1 - 0.55 * parent.parent.eased))
                    opacity: 0.72 * Math.pow(1 - parent.parent.phase, 1.35)
                }
            }
        }
    }

    // Built-in pointer, also used as a graceful fallback when a compositor did
    // not provide a recorded system bitmap.
    Image {
        visible: overlay._cursorVisible
                 && (overlay._style === "pointer"
                     || (overlay._style === "system" && !overlay._haveSystemShape))
        opacity: overlay._cursorOpacity
        source: "qrc:/resources/cursors/pointer.svg"
        sourceSize: Qt.size(Math.max(2, Math.round(overlay._ptrScreenW * overlay._ptrSuper)),
                            Math.max(2, Math.round(overlay._screenH * overlay._ptrSuper)))
        width: overlay._ptrW
        height: overlay._ptrH
        x: overlay._cursorX - overlay._ptrHotX
        y: overlay._cursorY - overlay._ptrHotY
        smooth: true
        mipmap: true
        cache: true
        transform: Scale {
            origin.x: overlay._ptrHotX
            origin.y: overlay._ptrHotY
            xScale: overlay._camW
            yScale: overlay._camH
        }
        layer.enabled: true
        // Render the shadow-capture FBO at the supersampled resolution, not the
        // item's on-screen size, so the pointer stays crisp when the camera or the
        // display magnifies the composition.
        layer.textureSize: Qt.size(Math.max(2, Math.round(overlay._ptrW * overlay._ptrSuper)),
                                   Math.max(2, Math.round(overlay._ptrH * overlay._ptrSuper)))
        layer.smooth: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Qt.rgba(0, 0, 0, 1)
            shadowBlur: 0.55
            // blurMax is an ABSOLUTE radius in source (FBO) pixels and defaults
            // to 32 — i.e. ~17.6 px of blur at shadowBlur 0.55 no matter how big
            // the pointer is. On a cursor only ~15 px tall that smeared the whole
            // arrow into a featureless grey square (the "blurry cursor" bug); it
            // only looked acceptable once the pointer was large enough to out-
            // scale it. Tie the radius to the pointer's own FBO height so the
            // shadow stays proportional — identical look at every cursor size,
            // zoom level and export resolution.
            blurMax: Math.max(2, Math.round(overlay._ptrH * overlay._ptrSuper * 0.30))
            shadowVerticalOffset: Math.max(0.5, overlay._ptrH * 0.035)
            shadowOpacity: 0.30
        }
    }

    // Recorded cursor bitmap. Normalize its display height to the premium pointer
    // while preserving its own aspect and exact recorded hotspot.
    Image {
        id: systemCursor
        readonly property real screenScale: overlay.cursorData
                                                ? overlay._screenH
                                                  / Math.max(1, overlay.cursorData.shapeHeight)
                                                : 1
        visible: overlay._cursorVisible && overlay._style === "system"
                 && overlay._haveSystemShape
        opacity: overlay._cursorOpacity
        source: overlay.cursorData ? overlay.cursorData.shapeUrl : ""
        sourceSize: overlay.cursorData
                    ? Qt.size(Math.round(overlay.cursorData.shapeWidth * overlay._sysSuper),
                              Math.round(overlay.cursorData.shapeHeight * overlay._sysSuper))
                    : Qt.size(0, 0)
        width: overlay.cursorData
               ? overlay.cursorData.shapeWidth * screenScale * overlay._pressScale : 0
        height: overlay.cursorData
                ? overlay.cursorData.shapeHeight * screenScale * overlay._pressScale : 0
        x: overlay._cursorX
           - (overlay.cursorData ? overlay.cursorData.hotspotX : 0)
             * screenScale * overlay._pressScale
        y: overlay._cursorY
           - (overlay.cursorData ? overlay.cursorData.hotspotY : 0)
             * screenScale * overlay._pressScale
        smooth: true
        mipmap: true
        cache: true
        transform: Scale {
            origin.x: (overlay.cursorData ? overlay.cursorData.hotspotX : 0)
                      * systemCursor.screenScale * overlay._pressScale
            origin.y: (overlay.cursorData ? overlay.cursorData.hotspotY : 0)
                      * systemCursor.screenScale * overlay._pressScale
            xScale: overlay._camW
            yScale: overlay._camH
        }
    }

    // Tasteful highlight cursor: translucent accent halo with a crisp light core.
    Item {
        id: highlightCursor
        readonly property real screenD: overlay._screenH * 0.72
        visible: overlay._cursorVisible && overlay._style === "dot"
        opacity: overlay._cursorOpacity
        x: overlay._cursorX - width / 2
        y: overlay._cursorY - height / 2
        width: screenD * overlay._pressScale
        height: screenD * overlay._pressScale
        transformOrigin: Item.Center
        transform: Scale {
            origin.x: highlightCursor.width / 2
            origin.y: highlightCursor.height / 2
            xScale: overlay._camW
            yScale: overlay._camH
        }
        Rectangle {
            anchors.fill: parent
            radius: Math.min(width, height) / 2
            color: overlay._rippleColor
            opacity: 0.42
        }
        Rectangle {
            anchors.centerIn: parent
            width: parent.width * 0.48
            height: parent.height * 0.48
            radius: Math.min(width, height) / 2
            color: Qt.rgba(1, 1, 1, 0.96)
        }
    }

    // Precision ring: accent outline plus a small center point, readable on both
    // light and dark footage without looking like debug geometry.
    Item {
        id: precisionCursor
        readonly property real screenD: overlay._screenH * 0.88
        visible: overlay._cursorVisible && overlay._style === "circle"
        opacity: overlay._cursorOpacity
        x: overlay._cursorX - width / 2
        y: overlay._cursorY - height / 2
        width: screenD * overlay._pressScale
        height: screenD * overlay._pressScale
        transformOrigin: Item.Center
        transform: Scale {
            origin.x: precisionCursor.width / 2
            origin.y: precisionCursor.height / 2
            xScale: overlay._camW
            yScale: overlay._camH
        }
        Rectangle {
            anchors.fill: parent
            radius: Math.min(width, height) / 2
            color: Qt.rgba(0, 0, 0, 0.16)
            border.color: overlay._rippleColor
            border.width: Math.max(0.8, parent.width * 0.12)
        }
        Rectangle {
            anchors.centerIn: parent
            width: parent.width * 0.16
            height: parent.height * 0.16
            radius: Math.min(width, height) / 2
            color: Qt.rgba(1, 1, 1, 0.94)
        }
    }
}
