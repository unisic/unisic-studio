import QtQuick
import Unisic.Kit

// On-canvas zoom-target editor: a draggable body plus 8 resize handles,
// aspect-locked to the OUTPUT aspect, writing the keyframe rect back to the
// model live. Parented into the composition's editorSlot by the editor window
// (never by the export renderer) — OUTSIDE the camera transform. The region of
// the source currently on screen is `viewRect` (source-normalized): identity in
// fit mode (whole frame), a zoomed-out output-aspect context region in fill
// mode. All box geometry maps source coords through it; with the identity view
// every formula reduces to the plain [0,1] mapping.
Item {
    id: editor

    // C++ StudioProject* + the selected row; -1 == nothing to edit.
    property var project: null
    property int index: -1
    // Output vs source pixel aspect. The on-screen box keeps the OUTPUT aspect, so
    // the locked normalized width/height ratio is outputAspect / sourceAspect.
    property real outputAspect: 16 / 9
    property real sourceAspect: 16 / 9
    readonly property real _ratioWH: sourceAspect > 0 ? outputAspect / sourceAspect : 1
    // The source region the composition is showing (must equal its _effZoom while
    // editing, or the box lands beside the video it annotates).
    property rect viewRect: Qt.rect(0, 0, 1, 1)
    readonly property real _vw: Math.max(0.0001, viewRect.width)
    readonly property real _vh: Math.max(0.0001, viewRect.height)

    readonly property var _kf: (project && index >= 0 && _rev >= 0)
                               ? project.zoom.keyframeAt(index) : null
    readonly property bool _valid: _kf && _kf.w !== undefined && _kf.w > 0
    property int _rev: 0

    // Live-edited normalized rect (kept smooth during a drag; committed each move).
    property real nx: 0
    property real ny: 0
    property real nw: 1
    property real nh: 1
    readonly property real _minW: 0.06

    // Emitted when the user clicks outside the crop box (a request to deselect).
    signal deselectRequested()

    visible: _valid

    // Background: a click off the box deselects the keyframe.
    MouseArea {
        anchors.fill: parent
        onClicked: editor.deselectRequested()
    }

    function pull() {
        if (_valid) { nx = _kf.x; ny = _kf.y; nw = _kf.w; nh = _kf.h }
    }
    function commit() {
        if (project && index >= 0)
            project.zoom.setKeyframeRect(index, Qt.rect(nx, ny, nw, nh))
    }
    function clampRect() {
        nw = Math.max(_minW, Math.min(nw, 1))
        nh = Math.max(_minW * _ratioWH, Math.min(nh, 1))
        nx = Math.max(0, Math.min(nx, 1 - nw))
        ny = Math.max(0, Math.min(ny, 1 - nh))
    }
    onIndexChanged: pull()
    Component.onCompleted: pull()
    Connections {
        target: (editor.project) ? editor.project.zoom : null
        ignoreUnknownSignals: true
        // Refresh from the model on external edits (inspector zoom / nudge), but not
        // while this editor is the one dragging (guarded by _dragging).
        function onChanged() { editor._rev++; if (!editor._dragging) editor.pull() }
    }
    property bool _dragging: false

    // Aspect-locked resize driven by a handle. mnx/mny are the pointer position in
    // normalized [0,1] source coords; the opposite side/corner (ax,ay) stays fixed.
    function resize(h, mnx, mny) {
        const ax = editor.nx + h.ax * editor.nw   // fixed anchor (pre-resize rect)
        const ay = editor.ny + h.ay * editor.nh
        let w = editor.nw, hgt = editor.nh
        if (h.drive === "y") {
            hgt = Math.abs(mny - ay)
            w = hgt * editor._ratioWH
        } else {
            w = Math.abs(mnx - ax)
            hgt = w / editor._ratioWH
        }
        w = Math.max(editor._minW, Math.min(w, 1))
        hgt = Math.max(editor._minW * editor._ratioWH, Math.min(hgt, 1))
        editor.nw = w
        editor.nh = hgt
        editor.nx = (h.ax > 0.5) ? ax - w : (h.ax < 0.5 ? ax : ax - w / 2)
        editor.ny = (h.ay > 0.5) ? ay - hgt : (h.ay < 0.5 ? ay : ay - hgt / 2)
        editor.clampRect()
        editor.commit()
    }

    // Dimming outside the crop box (four bands) so the target reads clearly.
    readonly property color _dim: "#000000"
    readonly property real _dimO: 0.42
    Rectangle {   // top
        color: editor._dim; opacity: editor._dimO
        x: 0; y: 0; width: editor.width; height: box.y
    }
    Rectangle {   // bottom
        color: editor._dim; opacity: editor._dimO
        x: 0; y: box.y + box.height; width: editor.width
        height: Math.max(0, editor.height - box.y - box.height)
    }
    Rectangle {   // left
        color: editor._dim; opacity: editor._dimO
        x: 0; y: box.y; width: box.x; height: box.height
    }
    Rectangle {   // right
        color: editor._dim; opacity: editor._dimO
        x: box.x + box.width; y: box.y
        width: Math.max(0, editor.width - box.x - box.width); height: box.height
    }

    Rectangle {
        id: box
        x: (editor.nx - editor.viewRect.x) / editor._vw * editor.width
        y: (editor.ny - editor.viewRect.y) / editor._vh * editor.height
        width: editor.nw / editor._vw * editor.width
        height: editor.nh / editor._vh * editor.height
        color: "transparent"
        border.color: Theme.accent
        border.width: 2
        z: 2

        // Move body.
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.SizeAllCursor
            property real ox: 0
            property real oy: 0
            onPressed: (m) => { editor._dragging = true; ox = m.x; oy = m.y }
            onReleased: { editor._dragging = false }
            onPositionChanged: (m) => {
                if (!pressed) return
                // Pixel delta -> source-normalized delta through the view scale.
                editor.nx += (m.x - ox) / editor.width * editor._vw
                editor.ny += (m.y - oy) / editor.height * editor._vh
                editor.clampRect()
                editor.commit()
            }
        }

        // 8 resize handles (4 corners + 4 edges), aspect-locked.
        Repeater {
            model: [
                { hx: 0,   hy: 0,   ax: 1,   ay: 1,   drive: "xy", cur: Qt.SizeFDiagCursor },
                { hx: 1,   hy: 0,   ax: 0,   ay: 1,   drive: "xy", cur: Qt.SizeBDiagCursor },
                { hx: 0,   hy: 1,   ax: 1,   ay: 0,   drive: "xy", cur: Qt.SizeBDiagCursor },
                { hx: 1,   hy: 1,   ax: 0,   ay: 0,   drive: "xy", cur: Qt.SizeFDiagCursor },
                { hx: 0.5, hy: 0,   ax: 0.5, ay: 1,   drive: "y",  cur: Qt.SizeVerCursor },
                { hx: 0.5, hy: 1,   ax: 0.5, ay: 0,   drive: "y",  cur: Qt.SizeVerCursor },
                { hx: 0,   hy: 0.5, ax: 1,   ay: 0.5, drive: "x",  cur: Qt.SizeHorCursor },
                { hx: 1,   hy: 0.5, ax: 0,   ay: 0.5, drive: "x",  cur: Qt.SizeHorCursor }
            ]
            delegate: Rectangle {
                required property var modelData
                width: 14
                height: 14
                radius: 3
                color: Theme.accent
                border.color: "#ffffff"
                border.width: 1
                x: modelData.hx * box.width - width / 2
                y: modelData.hy * box.height - height / 2
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: parent.modelData.cur
                    onPressed: editor._dragging = true
                    onReleased: editor._dragging = false
                    onPositionChanged: (m) => {
                        if (!pressed) return
                        // Pointer position in SOURCE-normalized coords (through the view).
                        var p = mapToItem(editor, m.x, m.y)
                        editor.resize(parent.modelData,
                                      p.x / Math.max(1, editor.width) * editor._vw
                                          + editor.viewRect.x,
                                      p.y / Math.max(1, editor.height) * editor._vh
                                          + editor.viewRect.y)
                    }
                }
            }
        }
    }
}
