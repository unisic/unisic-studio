import QtQuick

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
        readonly property real _d: overlay.width * 0.02 * overlay._cursorScale
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
