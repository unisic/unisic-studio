import QtQuick
import QtQuick.Window
import Unisic.Kit
import UnisicStudio

// Per-recording editor window. Created by C++ EditorWindowManager with its own
// QQmlContext carrying `editorProject` (a StudioProject*). Frameless + custom
// title bar (Unisic's pattern). Center: the shared CompositionRoot scene with a
// live PreviewVideo (or a poster-frame fallback when QtMultimedia is absent);
// right: the InspectorPanel bound to the project's StyleModel. Every string
// qsTr()'d, every color a Theme token.
Window {
    id: editorWindow
    width: 1200
    height: 780
    minimumWidth: 940
    minimumHeight: 560
    visible: true
    color: Theme.backgroundDeep

    // Frameless with a hand-built title bar (Unisic default; no system deco).
    flags: Qt.Window | Qt.FramelessWindowHint
    readonly property int chromeTop: 38

    // Set from C++ (EditorWindowManager) when the poster-frame fallback lands.
    property string posterSource: ""
    // Guards the close-confirm loop.
    property bool confirmedClose: false

    // Resolved playable path: resolved-or-abs (same fallback as C++ so a freshly
    // imported, not-yet-saved project still previews).
    readonly property string videoPath: editorProject
        ? (editorProject.videoResolved !== "" ? editorProject.videoResolved
                                               : editorProject.videoAbsPath)
        : ""
    readonly property string videoUrl: videoPath === "" ? "" : "file://" + encodeURI(videoPath)

    function baseName(p) {
        if (!p) return ""
        var i = p.lastIndexOf("/")
        return i >= 0 ? p.substring(i + 1) : p
    }
    function mmss(ms) {
        if (isNaN(ms) || ms < 0) ms = 0
        var s = Math.floor(ms / 1000)
        var m = Math.floor(s / 60)
        s = s % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    readonly property string projectName: editorProject && editorProject.videoAbsPath !== ""
                                          ? baseName(editorProject.videoAbsPath) : qsTr("Untitled")
    title: projectName + (editorProject && editorProject.dirty ? " •" : "")

    // Transport state (from the loaded player, or the project's known duration).
    readonly property int curPos: videoLoader.item ? videoLoader.item.positionMs : 0
    readonly property int curDur: videoLoader.item ? videoLoader.item.durationMs
                                  : (editorProject ? editorProject.durationMs : 0)
    readonly property bool hasPlayback: Studio.capVideoPlayback && videoLoader.item !== null

    // Trim range (mirrors the Timeline's effOut fallback). Preview playback is
    // confined to [trimInMs, trimOutMs]: play jumps into range, and the head
    // auto-pauses at the out point. Scrubbing anywhere stays free.
    readonly property int trimInMs: editorProject ? editorProject.trimInMs : 0
    readonly property int trimOutMs: (editorProject && editorProject.trimOutMs > trimInMs)
                                     ? editorProject.trimOutMs : curDur

    // The selected zoom keyframe (drives the inspector's keyframe section and the
    // timeline highlight); -1 == none. Row indices shift when keyframes are
    // added/removed, so a count change clears the selection.
    property int selectedKeyframe: -1

    // One seek entry point: with live playback it moves the player (whose
    // position feeds preview.sync); without it, it snaps the preview clock.
    function seekTo(ms) {
        if (hasPlayback) videoLoader.item.seek(ms)
        else if (typeof preview !== "undefined" && preview) preview.snap(ms)
    }

    // Start playback from the trim-in point when the head is outside the trimmed
    // range, then toggle. Keeps the preview confined to the exported span.
    function playPause() {
        if (!videoLoader.item) return
        if (!videoLoader.item.isPlaying) {
            var pos = videoLoader.item.positionMs
            if (pos < trimInMs || pos >= trimOutMs - 20)
                videoLoader.item.seek(trimInMs)
        }
        videoLoader.item.togglePlay()
    }

    // Re-anchor the smoothing clock whenever the player reports a new position or
    // toggles play/pause; the clock interpolates between these coarse updates.
    // While playing, auto-pause at the trim-out point so the preview matches the
    // exported range.
    Connections {
        target: videoLoader.item
        ignoreUnknownSignals: true
        function onPositionMsChanged() {
            var pos = videoLoader.item.positionMs
            if (videoLoader.item.isPlaying && editorWindow.trimOutMs > 0
                    && pos >= editorWindow.trimOutMs) {
                videoLoader.item.pause()
                videoLoader.item.seek(editorWindow.trimOutMs)
                preview.sync(editorWindow.trimOutMs, false)
                return
            }
            preview.sync(pos, videoLoader.item.isPlaying)
        }
        function onIsPlayingChanged() { preview.sync(videoLoader.item.positionMs, videoLoader.item.isPlaying) }
    }

    // Keyframe row indices are unstable across add/remove — drop the selection.
    Connections {
        target: editorProject ? editorProject.zoom : null
        ignoreUnknownSignals: true
        function onCountChanged() { editorWindow.selectedKeyframe = -1 }
    }

    // Ctrl+S saves (silent after first save).
    Shortcut {
        sequence: StandardKey.Save
        onActivated: if (editorProject) Studio.saveProject(editorProject)
    }

    onClosing: (close) => {
        if (editorProject && editorProject.dirty && !confirmedClose) {
            close.accepted = false
            discardDlg.open()
        }
    }

    // ---- Custom title bar (frameless) --------------------------------------
    Rectangle {
        id: titleBar
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: editorWindow.chromeTop
        z: 20
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.lighter(Theme.primary, 1.12) }
            GradientStop { position: 1.0; color: Theme.primary }
        }

        MouseArea {
            anchors.fill: parent
            property real pressX: 0
            property real pressY: 0
            property bool moving: false
            onPressed: (m) => { pressX = m.x; pressY = m.y; moving = false }
            onPositionChanged: (m) => {
                if (!moving && (Math.abs(m.x - pressX) > 6 || Math.abs(m.y - pressY) > 6)) {
                    moving = true
                    editorWindow.startSystemMove()
                }
            }
            onDoubleClicked: editorWindow.visibility === Window.Maximized
                             ? editorWindow.showNormal() : editorWindow.showMaximized()
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.spacingL
            anchors.verticalCenter: parent.verticalCenter
            text: editorWindow.title
            color: Theme.textPrimary
            font.pixelSize: Theme.fontM
            font.weight: Font.DemiBold
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            UIconButton {
                iconName: "minus"; iconSize: 14; width: 30; height: 30
                tooltip: qsTr("Minimize"); onClicked: editorWindow.showMinimized()
            }
            UIconButton {
                iconName: "window"; iconSize: 13; width: 30; height: 30
                tooltip: qsTr("Maximize")
                onClicked: editorWindow.visibility === Window.Maximized
                           ? editorWindow.showNormal() : editorWindow.showMaximized()
            }
            UIconButton {
                iconName: "close"; iconSize: 14; width: 30; height: 30
                tooltip: qsTr("Close"); onClicked: editorWindow.close()
            }
        }
    }

    // ---- Right: style inspector --------------------------------------------
    InspectorPanel {
        id: inspector
        width: 300
        anchors { top: titleBar.bottom; right: parent.right; bottom: parent.bottom }
        styleModel: editorProject ? editorProject.style : null
        selectedKeyframe: editorWindow.selectedKeyframe
        onDeselect: editorWindow.selectedKeyframe = -1
    }

    // ---- Left: toolbar + canvas + transport --------------------------------
    Item {
        id: leftPane
        anchors { top: titleBar.bottom; left: parent.left; right: inspector.left; bottom: parent.bottom }

        // Toolbar
        Item {
            id: toolbar
            anchors { top: parent.top; left: parent.left; right: parent.right }
            anchors.topMargin: Theme.spacingM
            anchors.leftMargin: Theme.spacingL
            anchors.rightMargin: Theme.spacingL
            height: 40

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: editorWindow.projectName
                color: Theme.textPrimary
                font.pixelSize: Theme.fontL
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                width: Math.min(implicitWidth, parent.width * 0.35)
            }

            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spacingM

                Row {
                    spacing: Theme.spacingS
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        text: qsTr("Aspect")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontS
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    UComboBox {
                        id: aspectCombo
                        width: 130
                        readonly property var values: ["source", "16:9", "9:16", "1:1"]
                        model: [qsTr("Source"), "16:9", "9:16", "1:1"]
                        currentIndex: editorProject && editorProject.style
                                      ? Math.max(0, values.indexOf(editorProject.style.aspect)) : 0
                        onActivated: (i) => {
                            if (editorProject && editorProject.style)
                                editorProject.style.aspect = values[i]
                        }
                    }
                }

                UIconButton {
                    iconName: "document-save"
                    tooltip: qsTr("Save (Ctrl+S)")
                    anchors.verticalCenter: parent.verticalCenter
                    onClicked: if (editorProject) Studio.saveProject(editorProject)
                }

                UButton {
                    anchors.verticalCenter: parent.verticalCenter
                    variant: "filled"
                    compact: true
                    text: qsTr("Export")
                    // One export at a time: disabled while the dialog is running.
                    enabled: editorProject !== null && !exportDialog.running
                    onClicked: {
                        exportDialog.project = editorProject
                        exportDialog.open()
                    }
                }
            }
        }

        // Canvas
        Item {
            id: canvasArea
            anchors {
                top: toolbar.bottom; left: parent.left; right: parent.right; bottom: timeline.top
                topMargin: Theme.spacingM
                bottomMargin: Theme.spacingM
            }

            CompositionRoot {
                id: comp
                anchors.fill: parent
                anchors.margins: Theme.spacingL
                styleModel: editorProject ? editorProject.style : null
                videoSize: editorProject ? editorProject.videoSize : Qt.size(1920, 1080)
                // Camera + cursor, both from the one preview head (evaluate is C++).
                zoomRect: (typeof preview !== "undefined" && preview) ? preview.zoomRect
                                                                      : Qt.rect(0, 0, 1, 1)
                cursorPlayback: (typeof preview !== "undefined" && preview) ? preview.cursor : null
                timeMs: (typeof preview !== "undefined" && preview) ? preview.timeMs : 0
                // Feeds the "desktopBlur" background (poster of the first frame).
                posterSource: editorWindow.posterSource
            }

            // Live video (only when QtMultimedia is present) parented into the
            // composition's slot.
            Loader {
                id: videoLoader
                parent: comp.videoSlot
                anchors.fill: parent
                active: Studio.capVideoPlayback && editorWindow.videoPath !== ""
                sourceComponent: PreviewVideo { source: editorWindow.videoUrl }
            }

            // Poster-frame fallback (no QtMultimedia). posterSource is filled by
            // C++ once ffmpeg extracts the still.
            Image {
                parent: comp.videoSlot
                anchors.fill: parent
                visible: !Studio.capVideoPlayback
                source: editorWindow.posterSource
                fillMode: Image.PreserveAspectFit
                Text {
                    anchors.centerIn: parent
                    visible: editorWindow.posterSource === ""
                    text: qsTr("Preview unavailable")
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontM
                }
            }

            // Canvas interaction: click = add/adjust a Manual zoom at the playhead
            // centred on the clicked point; double-click = reset to full frame. The
            // 220 ms timer keeps the single-click from firing on a double-click.
            MouseArea {
                id: canvasClick
                anchors.fill: parent
                enabled: editorProject !== null
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                function normAt(mx, my) {
                    var p = comp.videoSlot.mapFromItem(canvasClick, mx, my)
                    return Qt.point(p.x / Math.max(1, comp.videoSlot.width),
                                    p.y / Math.max(1, comp.videoSlot.height))
                }
                Timer {
                    id: addTimer
                    interval: 220
                    property real nx: 0.5
                    property real ny: 0.5
                    onTriggered: {
                        var row = Studio.addManualZoom(editorProject, Math.round(preview.timeMs),
                                                       nx, ny, 1.8)
                        if (row >= 0) editorWindow.selectedKeyframe = row
                    }
                }
                onClicked: (m) => {
                    var n = normAt(m.x, m.y)
                    if (n.x < 0 || n.x > 1 || n.y < 0 || n.y > 1) return  // outside video
                    addTimer.nx = n.x; addTimer.ny = n.y
                    addTimer.restart()
                }
                onDoubleClicked: (m) => {
                    addTimer.stop()
                    var row = Studio.addResetZoom(editorProject, Math.round(preview.timeMs))
                    if (row >= 0) editorWindow.selectedKeyframe = row
                }
            }

            // Help hint for the canvas gesture.
            Text {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.leftMargin: Theme.spacingS
                text: qsTr("Click: add zoom · Double-click: reset")
                color: Theme.textTertiary
                font.pixelSize: Theme.fontS
                opacity: canvasClick.containsMouse ? 0.9 : 0.0
                Behavior on opacity { NumberAnimation { duration: Theme.animFast } }
            }
        }

        // Timeline strip (ruler, playhead, trim, keyframes, clicks, actions).
        Timeline {
            id: timeline
            anchors { left: parent.left; right: parent.right; bottom: transport.top }
            anchors.leftMargin: Theme.spacingL
            anchors.rightMargin: Theme.spacingL
            anchors.bottomMargin: Theme.spacingS
            height: 110
            project: editorProject
            durationMs: editorWindow.curDur
            playheadMs: (typeof preview !== "undefined" && preview) ? preview.timeMs
                                                                    : editorWindow.curPos
            selectedIndex: editorWindow.selectedKeyframe
            onSeek: (ms) => editorWindow.seekTo(ms)
            onSelectKeyframe: (index) => editorWindow.selectedKeyframe = index
        }

        // Transport
        Item {
            id: transport
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            anchors.leftMargin: Theme.spacingL
            anchors.rightMargin: Theme.spacingL
            anchors.bottomMargin: Theme.spacingM
            height: 44

            UIconButton {
                id: playBtn
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                enabled: editorWindow.hasPlayback
                iconName: (videoLoader.item && videoLoader.item.isPlaying)
                          ? "media-playback-pause" : "media-playback-start"
                tooltip: qsTr("Play / pause")
                onClicked: editorWindow.playPause()
            }

            USlider {
                id: seek
                anchors.left: playBtn.right
                anchors.right: timeLabel.left
                anchors.leftMargin: Theme.spacingM
                anchors.rightMargin: Theme.spacingM
                anchors.verticalCenter: parent.verticalCenter
                enabled: editorWindow.hasPlayback
                from: 0
                to: Math.max(1, editorWindow.curDur)
                value: editorWindow.curPos
                onMoved: (v) => editorWindow.seekTo(v)
            }

            Text {
                id: timeLabel
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                // When trimmed, append the exported clip length so the range is
                // legible without reading the timeline handles.
                readonly property bool trimmed: editorWindow.trimInMs > 0
                    || editorWindow.trimOutMs < editorWindow.curDur
                text: editorWindow.mmss(editorWindow.curPos) + " / " + editorWindow.mmss(editorWindow.curDur)
                      + (trimmed ? "  ·  " + qsTr("clip %1").arg(
                                       editorWindow.mmss(editorWindow.trimOutMs - editorWindow.trimInMs))
                                 : "")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontS
            }
        }
    }

    // Export modal (owns its own ExportController + offscreen decode/render).
    ExportDialog {
        id: exportDialog
    }

    // Dev-only smoke test (also bound in the main window) — reachable from the
    // editor where the export path is exercised.
    Shortcut {
        sequence: "F8"
        enabled: Studio.devBuild
        onActivated: smokeDialog.run()
    }
    SmokeTestDialog { id: smokeDialog }

    // Themed confirm on closing a dirty project (kit dialog, not QMessageBox).
    UConfirmDialog {
        id: discardDlg
        title: qsTr("Discard unsaved changes?")
        text: qsTr("Your changes will be lost. Save first with Ctrl+S.")
        confirmText: qsTr("Discard")
        cancelText: qsTr("Keep editing")
        destructive: true
        onAccepted: { editorWindow.confirmedClose = true; editorWindow.close() }
    }
}
