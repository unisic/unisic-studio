import QtQuick
import QtQuick.Effects

// The cursor + click-ripple layer, drawn ON the video and INSIDE the zoom
// transform (it is a child of CompositionRoot's videoZoom), so it scales and pans
// with the camera exactly like the footage. The recording was captured in
// Metadata mode — no baked-in pointer — so without this the cursor is invisible.
//
// Self-contained like the rest of the composition: no Theme, no app singletons.
// Every input arrives through a property. `cursorData` is a CursorPlayback*
// (normalised position, current shape bitmap, active ripples); `styleModel` is
// the project StyleModel (cursor style/scale, ripple on/off + colour); videoSize
// is the source pixel size, used to map recorded stream px -> this item's coords.
Item {
    id: overlay

    property var cursorData: null       // CursorPlayback*
    property var styleModel: null       // StyleModel*
    property size videoSize: Qt.size(1920, 1080)
    // The overlay's own time (from cursorData) drives ripple phase; exposed so
    // export's per-frame property writes re-trigger the bindings.
    readonly property real timeMs: cursorData ? cursorData.timeMs : 0

    // Stream px -> local px (before the zoom transform, which is applied by the
    // parent). videoZoom has the source aspect exactly, so this is uniform.
    readonly property real _dispScale: videoSize.width > 0 ? width / videoSize.width : 1
    readonly property real _cursorScale: styleModel ? styleModel.cursorScale : 1
    readonly property string _style: styleModel ? styleModel.cursorStyle : "system"
    readonly property color _rippleColor: styleModel ? styleModel.rippleColor : "#C8ACD6"
    readonly property bool _rippleOn: styleModel ? styleModel.clickRipple : true

    readonly property real _cursorX: cursorData ? cursorData.nx * width : 0
    readonly property real _cursorY: cursorData ? cursorData.ny * height : 0
    readonly property bool _cursorVisible: cursorData ? cursorData.cursorVisible : false

    // Subtle 'press' dip: the cursor scales to 0.9 the instant a click lands and
    // eases back over 120 ms. A pure function of msSinceClick (deterministic in
    // time), so preview and export dip identically. Only when ripples are enabled.
    readonly property real _pressScale: {
        if (!_rippleOn || !cursorData) return 1.0
        var ms = cursorData.msSinceClick
        if (ms < 0 || ms >= 120) return 1.0
        return 0.9 + 0.1 * (ms / 120)
    }

    // Built-in 'pointer' arrow geometry (SVG authored in a 20x26 box, tip at
    // (1.5,1.5)). Rasterised at the un-pressed size so a press doesn't re-raster.
    readonly property real _ptrBaseH: 28 * _dispScale * _cursorScale
    readonly property real _ptrH: _ptrBaseH * _pressScale
    readonly property real _ptrW: _ptrH * (20 / 26)
    readonly property real _ptrHotX: _ptrW * (1.5 / 20)
    readonly property real _ptrHotY: _ptrH * (1.5 / 26)

    // ---- Click ripples (deterministic: phase == (time - clickTime)/rippleMs) --
    Repeater {
        model: overlay.cursorData ? overlay.cursorData.ripples : null
        delegate: Item {
            required property real nx
            required property real ny
            required property real tMs
            readonly property real _phase: {
                if (!overlay.cursorData) return 1
                var p = (overlay.timeMs - tMs) / overlay.cursorData.rippleMs
                return p < 0 ? 0 : (p > 1 ? 1 : p)
            }
            visible: overlay._rippleOn && _phase < 1
            Rectangle {
                readonly property real _d: overlay.width * (0.02 + 0.07 * parent._phase)
                x: parent.nx * overlay.width - _d / 2
                y: parent.ny * overlay.height - _d / 2
                width: _d
                height: _d
                radius: _d / 2
                color: "transparent"
                border.color: overlay._rippleColor
                border.width: Math.max(1, overlay.width * 0.004 * (1 - parent._phase))
                opacity: 0.6 * (1 - parent._phase)
            }
        }
    }

    // ---- The pointer ---------------------------------------------------------
    // Default 'pointer' style: a polished built-in vector arrow (macOS-like: white
    // fill, dark outline, subtle drop shadow), NEVER the recorded bitmap. Its tip
    // (the SVG hotspot) sits on the smoothed cursor position.
    Image {
        visible: overlay._cursorVisible && overlay._style === "pointer"
        source: "qrc:/resources/cursors/pointer.svg"
        // Rasterise crisp at the un-pressed display size (stable across a press).
        sourceSize: Qt.size(Math.max(2, Math.round(overlay._ptrBaseH * 20 / 26)),
                            Math.max(2, Math.round(overlay._ptrBaseH)))
        width: overlay._ptrW
        height: overlay._ptrH
        x: overlay._cursorX - overlay._ptrHotX
        y: overlay._cursorY - overlay._ptrHotY
        smooth: true
        cache: true
        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: "#000000"
            shadowBlur: 0.6
            shadowVerticalOffset: Math.max(1, overlay._ptrH * 0.03)
            shadowHorizontalOffset: 0
            shadowOpacity: 0.35
        }
    }

    // System style: the recorded bitmap, anchored at its hotspot.
    Image {
        visible: overlay._cursorVisible && overlay._style === "system"
                 && overlay.cursorData && overlay.cursorData.shapeUrl !== ""
        source: overlay.cursorData ? overlay.cursorData.shapeUrl : ""
        // Native px are stream px; render crisp at source resolution.
        sourceSize: overlay.cursorData
                    ? Qt.size(overlay.cursorData.shapeWidth, overlay.cursorData.shapeHeight)
                    : Qt.size(0, 0)
        width: overlay.cursorData
               ? overlay.cursorData.shapeWidth * overlay._dispScale * overlay._cursorScale : 0
        height: overlay.cursorData
                ? overlay.cursorData.shapeHeight * overlay._dispScale * overlay._cursorScale : 0
        x: overlay._cursorX
           - (overlay.cursorData ? overlay.cursorData.hotspotX : 0)
             * overlay._dispScale * overlay._cursorScale
        y: overlay._cursorY
           - (overlay.cursorData ? overlay.cursorData.hotspotY : 0)
             * overlay._dispScale * overlay._cursorScale
        smooth: true
        cache: false
    }

    // Dot / circle style: a synthetic pointer in the ripple-derived colour.
    Rectangle {
        readonly property real _d: overlay.width * 0.02 * overlay._cursorScale * overlay._pressScale
        visible: overlay._cursorVisible
                 && (overlay._style === "dot" || overlay._style === "circle")
        x: overlay._cursorX - _d / 2
        y: overlay._cursorY - _d / 2
        width: _d
        height: _d
        radius: _d / 2
        color: overlay._style === "dot" ? overlay._rippleColor : "transparent"
        border.color: overlay._rippleColor
        border.width: Math.max(1, _d * (overlay._style === "circle" ? 0.16 : 0.08))
    }
}
