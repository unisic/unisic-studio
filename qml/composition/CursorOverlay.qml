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
    readonly property real _pressScale: cursorData ? cursorData.pressScale : 1

    // Full compensation would keep size exactly constant. Retaining 8% of camera
    // zoom gives subtle emphasis without producing a giant pointer at 2.8x.
    readonly property real _zoomGrowth: Math.pow(Math.max(1, cameraZoom), 0.08)
    readonly property real _screenH: Math.max(18, Math.min(width, height) * 0.024)
                                     * _cursorScale * _zoomGrowth
    readonly property real _camW: Math.max(0.0001, cameraRect.width)
    readonly property real _camH: Math.max(0.0001, cameraRect.height)

    // Premium pointer SVG: 24x32, hotspot (2,1.5). Width/height are authored in
    // pre-camera local units; after parent scaling they resolve to _screenH.
    readonly property real _ptrScreenW: _screenH * (24 / 32)
    readonly property real _ptrW: _ptrScreenW * _camW * _pressScale
    readonly property real _ptrH: _screenH * _camH * _pressScale
    readonly property real _ptrHotX: _ptrW * (2 / 24)
    readonly property real _ptrHotY: _ptrH * (1.5 / 32)
    readonly property bool _haveSystemShape: cursorData && cursorData.shapeUrl !== ""

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
            readonly property real localW: screenD * overlay._camW
            readonly property real localH: screenD * overlay._camH
            visible: overlay._rippleOn && phase < 1

            Rectangle {
                x: parent.nx * overlay.width - parent.localW / 2
                y: parent.ny * overlay.height - parent.localH / 2
                width: parent.localW
                height: parent.localH
                radius: Math.min(width, height) / 2
                color: overlay._rippleColor
                opacity: 0.10 * Math.pow(1 - parent.phase, 2)
            }
            Rectangle {
                x: parent.nx * overlay.width - parent.localW / 2
                y: parent.ny * overlay.height - parent.localH / 2
                width: parent.localW
                height: parent.localH
                radius: Math.min(width, height) / 2
                color: "transparent"
                border.color: overlay._rippleColor
                border.width: Math.max(0.7 * overlay._camW,
                                       Math.min(overlay.width, overlay.height) * 0.0028
                                       * overlay._camW * (1 - 0.55 * parent.eased))
                opacity: 0.72 * Math.pow(1 - parent.phase, 1.35)
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
        sourceSize: Qt.size(Math.max(2, Math.round(overlay._ptrScreenW * 2)),
                            Math.max(2, Math.round(overlay._screenH * 2)))
        width: overlay._ptrW
        height: overlay._ptrH
        x: overlay._cursorX - overlay._ptrHotX
        y: overlay._cursorY - overlay._ptrHotY
        smooth: true
        mipmap: true
        cache: true
        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Qt.rgba(0, 0, 0, 1)
            shadowBlur: 0.55
            shadowVerticalOffset: Math.max(0.5 * overlay._camH,
                                           overlay._ptrH * 0.035)
            shadowOpacity: 0.30
        }
    }

    // Recorded cursor bitmap. Normalize its display height to the premium pointer
    // while preserving its own aspect and exact recorded hotspot.
    Image {
        readonly property real screenScale: overlay.cursorData
                                                ? overlay._screenH
                                                  / Math.max(1, overlay.cursorData.shapeHeight)
                                                : 1
        visible: overlay._cursorVisible && overlay._style === "system"
                 && overlay._haveSystemShape
        opacity: overlay._cursorOpacity
        source: overlay.cursorData ? overlay.cursorData.shapeUrl : ""
        sourceSize: overlay.cursorData
                    ? Qt.size(overlay.cursorData.shapeWidth * 2,
                              overlay.cursorData.shapeHeight * 2)
                    : Qt.size(0, 0)
        width: overlay.cursorData
               ? overlay.cursorData.shapeWidth * screenScale * overlay._camW
                 * overlay._pressScale : 0
        height: overlay.cursorData
                ? overlay.cursorData.shapeHeight * screenScale * overlay._camH
                  * overlay._pressScale : 0
        x: overlay._cursorX
           - (overlay.cursorData ? overlay.cursorData.hotspotX : 0)
             * screenScale * overlay._camW * overlay._pressScale
        y: overlay._cursorY
           - (overlay.cursorData ? overlay.cursorData.hotspotY : 0)
             * screenScale * overlay._camH * overlay._pressScale
        smooth: true
        mipmap: true
        cache: true
    }

    // Tasteful highlight cursor: translucent accent halo with a crisp light core.
    Item {
        readonly property real screenD: overlay._screenH * 0.72
        visible: overlay._cursorVisible && overlay._style === "dot"
        opacity: overlay._cursorOpacity
        x: overlay._cursorX - width / 2
        y: overlay._cursorY - height / 2
        width: screenD * overlay._camW * overlay._pressScale
        height: screenD * overlay._camH * overlay._pressScale
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
        readonly property real screenD: overlay._screenH * 0.88
        visible: overlay._cursorVisible && overlay._style === "circle"
        opacity: overlay._cursorOpacity
        x: overlay._cursorX - width / 2
        y: overlay._cursorY - height / 2
        width: screenD * overlay._camW * overlay._pressScale
        height: screenD * overlay._camH * overlay._pressScale
        Rectangle {
            anchors.fill: parent
            radius: Math.min(width, height) / 2
            color: Qt.rgba(0, 0, 0, 0.16)
            border.color: overlay._rippleColor
            border.width: Math.max(0.8 * overlay._camW, parent.width * 0.12)
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
