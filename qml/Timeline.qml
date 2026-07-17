import QtQuick
import Unisic.Kit

// The editor's bottom timeline strip: a time ruler, a draggable playhead synced
// with the preview, trim in/out handles, the ZoomTimeline's keyframes as
// draggable blocks (Auto vs Manual tinted, locked glyphed), click markers, and
// the Regenerate / + Zoom actions. Colors are Theme tokens; controls are kit
// components. Drives the preview through seek()/selectKeyframe() and the model
// through Studio's zoom invokables.
Rectangle {
    id: timeline
    color: Theme.backgroundDeep

    property var project: null
    property real durationMs: 1
    property real playheadMs: 0
    property int selectedIndex: -1

    signal seek(int ms)
    signal selectKeyframe(int index)

    readonly property var zoom: project ? project.zoom : null
    readonly property real effOutMs: project && project.trimOutMs > project.trimInMs
                                     ? project.trimOutMs : durationMs

    // ---- Geometry helpers ----------------------------------------------------
    readonly property real trackLeft: Theme.spacingL
    readonly property real trackRight: width - Theme.spacingL
    readonly property real trackW: Math.max(1, trackRight - trackLeft)
    function xForMs(ms) { return trackLeft + (durationMs > 0 ? ms / durationMs : 0) * trackW }
    function msForX(x) {
        var r = (x - trackLeft) / trackW
        return Math.max(0, Math.min(durationMs, r * durationMs))
    }
    function mmss(ms) {
        if (isNaN(ms) || ms < 0) ms = 0
        var s = Math.floor(ms / 1000), m = Math.floor(s / 60)
        s = s % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }
    function tickStep() {
        var steps = [500, 1000, 2000, 5000, 10000, 15000, 30000, 60000, 120000, 300000, 600000]
        for (var i = 0; i < steps.length; i++)
            if (steps[i] / Math.max(1, durationMs) * trackW >= 64) return steps[i]
        return steps[steps.length - 1]
    }

    // ---- Top actions row -----------------------------------------------------
    Row {
        id: actions
        anchors { top: parent.top; left: parent.left; right: parent.right }
        anchors.topMargin: Theme.spacingS
        anchors.leftMargin: Theme.spacingL
        anchors.rightMargin: Theme.spacingL
        height: 30
        spacing: Theme.spacingM
        z: 5

        ToolChip {
            iconName: "view-refresh"
            label: qsTr("Regenerate auto-zoom")
            enabled: timeline.project !== null
            onClicked: regenDlg.open()
        }
        UButton {
            variant: "tonal"
            compact: true
            text: qsTr("+ Zoom")
            enabled: timeline.project !== null
            anchors.verticalCenter: parent.verticalCenter
            onClicked: {
                var cx = 0.5, cy = 0.5
                if (typeof preview !== "undefined" && preview && preview.cursor
                        && preview.cursor.cursorVisible) {
                    cx = preview.cursor.nx; cy = preview.cursor.ny
                }
                var row = Studio.addManualZoom(timeline.project, Math.round(timeline.playheadMs),
                                               cx, cy, 1.8)
                if (row >= 0) timeline.selectKeyframe(row)
            }
        }
        Item { width: 1; height: 1 }   // spacer eaten by Row; label goes right
    }

    Text {
        anchors.right: parent.right
        anchors.rightMargin: Theme.spacingL
        anchors.verticalCenter: actions.verticalCenter
        text: timeline.zoom ? qsTr("%n zoom point(s)", "", timeline.zoom.count) : ""
        color: Theme.textTertiary
        font.pixelSize: Theme.fontS
        z: 5
    }

    // ---- Ruler ---------------------------------------------------------------
    Item {
        id: ruler
        anchors { top: actions.bottom; left: parent.left; right: parent.right }
        anchors.topMargin: 2
        height: 16
        Repeater {
            model: timeline.durationMs > 0 ? Math.floor(timeline.durationMs / timeline.tickStep()) + 1 : 0
            delegate: Item {
                required property int index
                readonly property real ms: index * timeline.tickStep()
                x: timeline.xForMs(ms)
                Rectangle { width: 1; height: 6; color: Theme.divider; y: 10 }
                Text {
                    y: -2
                    text: timeline.mmss(parent.ms)
                    color: Theme.textTertiary
                    font.pixelSize: 9
                }
            }
        }
    }

    // ---- Track (trim / clicks / keyframes / playhead) ------------------------
    Item {
        id: track
        anchors { top: ruler.bottom; left: parent.left; right: parent.right; bottom: parent.bottom }
        anchors.topMargin: 2
        anchors.bottomMargin: Theme.spacingS

        // Base rail.
        Rectangle {
            id: rail
            anchors.verticalCenter: parent.verticalCenter
            x: timeline.trackLeft
            width: timeline.trackW
            height: 40
            radius: Theme.radiusS
            color: Theme.surface
            border.width: 1
            border.color: Theme.divider
        }

        // Trim dimming (outside [trimIn, effOut]).
        Rectangle {
            anchors.top: rail.top; anchors.bottom: rail.bottom
            x: rail.x
            width: Math.max(0, timeline.xForMs(timeline.project ? timeline.project.trimInMs : 0) - rail.x)
            color: Qt.rgba(0, 0, 0, 0.45)
            radius: Theme.radiusS
        }
        Rectangle {
            anchors.top: rail.top; anchors.bottom: rail.bottom
            x: timeline.xForMs(timeline.effOutMs)
            width: Math.max(0, rail.x + rail.width - x)
            color: Qt.rgba(0, 0, 0, 0.45)
            radius: Theme.radiusS
        }

        // Background scrub (below blocks so blocks win the click).
        MouseArea {
            anchors.fill: parent
            onPressed: (m) => timeline.seek(Math.round(timeline.msForX(m.x)))
            onPositionChanged: (m) => { if (pressed) timeline.seek(Math.round(timeline.msForX(m.x))) }
        }

        // Click markers.
        Repeater {
            model: timeline.project ? timeline.project.clickDownTimesMs() : []
            delegate: Rectangle {
                required property var modelData
                width: 5; height: 5; radius: 2.5
                x: timeline.xForMs(modelData) - 2.5
                y: rail.y + rail.height - 8
                color: Theme.accent
                opacity: 0.7
            }
        }

        // Keyframe blocks.
        Repeater {
            model: timeline.zoom
            delegate: Rectangle {
                id: block
                required property int index
                required property real tMs
                required property int source
                required property bool locked
                readonly property bool isSel: timeline.selectedIndex === index
                property bool dragging: false
                property real dragX: 0

                width: 12
                height: 26
                radius: Theme.radiusS
                y: rail.y + (rail.height - height) / 2
                x: (dragging ? dragX : timeline.xForMs(tMs)) - width / 2
                // Auto = tertiary tint, Manual = secondary; selected = accent ring.
                color: source === 1 ? Theme.secondary : Theme.tertiary
                border.width: isSel ? 2 : 1
                border.color: isSel ? Theme.accent : Theme.divider

                UIcon {
                    visible: block.locked
                    anchors.centerIn: parent
                    name: "lock"
                    size: 10
                    color: Theme.textPrimary
                }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -3
                    onPressed: (m) => {
                        timeline.selectKeyframe(block.index)
                        block.dragX = block.x + block.width / 2
                    }
                    onPositionChanged: (m) => {
                        if (!pressed) return
                        block.dragging = true
                        block.dragX = Math.max(timeline.trackLeft,
                                               Math.min(timeline.trackRight, block.x + m.x))
                    }
                    onReleased: {
                        if (block.dragging && timeline.zoom) {
                            var row = timeline.zoom.moveKeyframe(block.index,
                                          Math.round(timeline.msForX(block.dragX)))
                            block.dragging = false
                            if (row >= 0) timeline.selectKeyframe(row)
                        }
                    }
                }
            }
        }

        // Trim handles.
        Rectangle {
            id: trimIn
            width: 8; radius: 3
            anchors.top: rail.top; anchors.bottom: rail.bottom
            x: timeline.xForMs(timeline.project ? timeline.project.trimInMs : 0) - width / 2
            color: Theme.accent
            MouseArea {
                anchors.fill: parent; anchors.margins: -4
                cursorShape: Qt.SizeHorCursor
                onPositionChanged: (m) => {
                    if (!pressed || !timeline.project) return
                    var v = Math.round(timeline.msForX(trimIn.x + width / 2 + m.x))
                    timeline.project.trimInMs = Math.min(v, timeline.effOutMs - 100)
                }
            }
        }
        Rectangle {
            id: trimOut
            width: 8; radius: 3
            anchors.top: rail.top; anchors.bottom: rail.bottom
            x: timeline.xForMs(timeline.effOutMs) - width / 2
            color: Theme.accent
            MouseArea {
                anchors.fill: parent; anchors.margins: -4
                cursorShape: Qt.SizeHorCursor
                onPositionChanged: (m) => {
                    if (!pressed || !timeline.project) return
                    var v = Math.round(timeline.msForX(trimOut.x + width / 2 + m.x))
                    timeline.project.trimOutMs = Math.max(v, (timeline.project.trimInMs || 0) + 100)
                }
            }
        }

        // Playhead (draggable).
        Rectangle {
            id: playhead
            width: 2
            anchors.top: parent.top; anchors.bottom: parent.bottom
            x: timeline.xForMs(timeline.playheadMs) - 1
            color: Theme.accent
            Rectangle {
                width: 10; height: 10; radius: 5
                anchors.horizontalCenter: parent.horizontalCenter
                y: -3
                color: Theme.accent
            }
            MouseArea {
                anchors.fill: parent; anchors.margins: -5
                cursorShape: Qt.SizeHorCursor
                onPositionChanged: (m) => {
                    if (pressed) timeline.seek(Math.round(timeline.msForX(playhead.x + 1 + m.x)))
                }
            }
        }
    }

    UConfirmDialog {
        id: regenDlg
        title: qsTr("Regenerate automatic zoom?")
        text: qsTr("Replaces automatic zoom points; manual and locked ones stay.")
        confirmText: qsTr("Regenerate")
        cancelText: qsTr("Cancel")
        onAccepted: if (timeline.project) Studio.regenerateZoom(timeline.project)
    }
}
