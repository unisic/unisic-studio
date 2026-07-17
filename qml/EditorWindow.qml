import QtQuick
import QtQuick.Window
import QtQuick.Controls
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
    // Optional webcam sidecar (composited as a corner overlay in the preview).
    readonly property string webcamUrl: (editorProject && editorProject.hasWebcam)
        ? "file://" + encodeURI(editorProject.webcamResolved) : ""

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

    // ---- Keyboard-driven editing -------------------------------------------
    // The smoothed preview clock is the shared playhead (timeline + transport read
    // it too); arrow keys step relative to it.
    readonly property int headMs: (typeof preview !== "undefined" && preview) ? preview.timeMs : curPos
    readonly property real fps: (editorProject && editorProject.fps > 0) ? editorProject.fps : 30
    readonly property int frameStepMs: Math.max(1, Math.round(1000 / fps))
    // True while a text field owns focus — the character/navigation shortcuts stand
    // down so typing a frame title or export filename isn't hijacked by Space/K/etc.
    readonly property bool typingText: activeFocusItem instanceof TextInput

    function stepBy(ms) { seekTo(Math.max(0, Math.min(curDur, headMs + ms))) }

    // Delete the selected keyframe unless it is locked (locked keyframes survive
    // regenerate; deleting one by a stray keypress would be surprising).
    function deleteSelectedKeyframe() {
        if (selectedKeyframe < 0 || !editorProject || !editorProject.zoom) return
        var m = editorProject.zoom.keyframeAt(selectedKeyframe)
        if (m && m.locked) return
        var i = selectedKeyframe
        selectedKeyframe = -1
        editorProject.zoom.removeAt(i)
    }

    // 'K' — add a Manual zoom at the playhead, centred on the cursor when visible
    // (same as the timeline's "+ Zoom").
    function addZoomAtHead() {
        if (!editorProject) return
        var cx = 0.5, cy = 0.5
        if (typeof preview !== "undefined" && preview && preview.cursor && preview.cursor.cursorVisible) {
            cx = preview.cursor.nx; cy = preview.cursor.ny
        }
        var row = Studio.addManualZoom(editorProject, Math.round(headMs), cx, cy, 1.8)
        if (row >= 0) selectedKeyframe = row
    }

    // Output pixel aspect (drives the on-canvas crop editor's aspect lock).
    readonly property real srcAspect: (editorProject && editorProject.videoSize.height > 0)
        ? editorProject.videoSize.width / editorProject.videoSize.height : (16 / 9)
    readonly property real outputAspect: {
        switch (editorProject && editorProject.style ? editorProject.style.aspect : "source") {
        case "16:9": return 16 / 9
        case "9:16": return 9 / 16
        case "1:1":  return 1
        default:     return srcAspect
        }
    }

    // Selecting a keyframe seeks to its instant so the on-canvas crop editor shows
    // the source frame at that moment (the composition is forced to full-frame).
    onSelectedKeyframeChanged: {
        if (selectedKeyframe >= 0 && editorProject && editorProject.zoom) {
            var m = editorProject.zoom.keyframeAt(selectedKeyframe)
            if (m && m.tMs !== undefined)
                seekTo(m.tMs)
        }
    }

    // One seek entry point: with live playback it moves the player (whose
    // position feeds preview.sync); without it, it snaps the preview clock.
    function seekTo(ms) {
        if (hasPlayback) videoLoader.item.seek(ms)
        else if (typeof preview !== "undefined" && preview) preview.snap(ms)
        if (webcamLoader.item) webcamLoader.item.seek(ms) // keep the overlay aligned
    }

    // Start playback from the trim-in point when the head is outside the trimmed
    // range, then toggle. Keeps the preview confined to the exported span.
    function playPause() {
        if (!videoLoader.item) return
        if (!videoLoader.item.isPlaying) {
            var pos = videoLoader.item.positionMs
            if (pos < trimInMs || pos >= trimOutMs - 20) {
                videoLoader.item.seek(trimInMs)
                if (webcamLoader.item) webcamLoader.item.seek(trimInMs)
            }
        }
        videoLoader.item.togglePlay()
        // Match the overlay's play state to the main player.
        if (webcamLoader.item) {
            if (videoLoader.item.isPlaying) webcamLoader.item.play()
            else webcamLoader.item.pause()
        }
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
                if (webcamLoader.item) webcamLoader.item.pause()
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

    // ---- Keyboard shortcuts -------------------------------------------------
    // Active when the editor window holds focus; character/navigation bindings are
    // suppressed while a text field is being edited (typingText).
    Shortcut { // Ctrl+S saves (silent after first save).
        sequence: StandardKey.Save
        onActivated: if (editorProject) Studio.saveProject(editorProject)
    }
    Shortcut {
        sequence: "Space"
        enabled: !editorWindow.typingText
        onActivated: editorWindow.playPause()
    }
    Shortcut {
        sequence: "Left"
        enabled: !editorWindow.typingText
        onActivated: editorWindow.stepBy(-editorWindow.frameStepMs)
    }
    Shortcut {
        sequence: "Right"
        enabled: !editorWindow.typingText
        onActivated: editorWindow.stepBy(editorWindow.frameStepMs)
    }
    Shortcut {
        sequence: "Shift+Left"
        enabled: !editorWindow.typingText
        onActivated: editorWindow.stepBy(-1000)
    }
    Shortcut {
        sequence: "Shift+Right"
        enabled: !editorWindow.typingText
        onActivated: editorWindow.stepBy(1000)
    }
    Shortcut {
        sequence: "Home"
        enabled: !editorWindow.typingText
        onActivated: editorWindow.seekTo(editorWindow.trimInMs)
    }
    Shortcut {
        sequence: "End"
        enabled: !editorWindow.typingText
        onActivated: editorWindow.seekTo(editorWindow.trimOutMs)
    }
    Shortcut {
        sequences: [StandardKey.Delete, StandardKey.Backspace]
        enabled: !editorWindow.typingText && editorWindow.selectedKeyframe >= 0
        onActivated: editorWindow.deleteSelectedKeyframe()
    }
    Shortcut {
        sequence: "K"
        enabled: !editorWindow.typingText && editorProject !== null
        onActivated: editorWindow.addZoomAtHead()
    }
    Shortcut { // Esc deselects the current keyframe (dialogs handle their own Esc).
        sequence: StandardKey.Cancel
        enabled: editorWindow.selectedKeyframe >= 0
        onActivated: editorWindow.selectedKeyframe = -1
    }
    Shortcut { // Ctrl+E opens the export dialog.
        sequence: "Ctrl+E"
        enabled: editorProject !== null && !exportDialog.running
        onActivated: { exportDialog.project = editorProject; exportDialog.open() }
    }
    Shortcut { // '?' / Ctrl+/ / F1 — the shortcut cheat sheet.
        sequences: ["?", "Ctrl+/", "F1"]
        onActivated: shortcutsPopup.open()
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
                            if (editorProject && editorProject.style) {
                                editorProject.style.aspect = values[i]
                                // Re-frame the auto camera to the new aspect (crop-to-
                                // fill follows the action); manual/locked kfs are kept.
                                Studio.regenerateZoom(editorProject)
                            }
                        }
                    }
                }

                UIconButton {
                    // '?' glyph fallback (no iconName) so it's always visible.
                    icon: "?"
                    tooltip: qsTr("Keyboard shortcuts (?)")
                    anchors.verticalCenter: parent.verticalCenter
                    onClicked: shortcutsPopup.open()
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
                // A webcam feed is present only when one was recorded, the module
                // is available, and the style enables it.
                webcamHasFeed: webcamLoader.item !== null
                // While a keyframe is selected, show the whole source frame so the
                // on-canvas crop editor maps 1:1 to the video card.
                editRect: editorWindow.selectedKeyframe >= 0
            }

            // On-canvas zoom-target editor for the selected keyframe. Parented into
            // the video slot (source-frame [0,1] coords, tracks the card); shown for
            // timeline selection, '+ Zoom' and canvas-click alike. Editor-only —
            // the export renderer instantiates CompositionRoot without it.
            Loader {
                parent: comp.videoSlot
                anchors.fill: parent
                z: 100
                active: editorProject !== null && editorWindow.selectedKeyframe >= 0
                sourceComponent: ZoomRectEditor {
                    project: editorProject
                    index: editorWindow.selectedKeyframe
                    sourceAspect: editorWindow.srcAspect
                    outputAspect: editorWindow.outputAspect
                    onDeselectRequested: editorWindow.selectedKeyframe = -1
                }
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

            // Webcam overlay feed parented into the composition's webcam slot.
            // Its position follows the main player (coarse sync on each update);
            // play/pause is driven by the same transport.
            Loader {
                id: webcamLoader
                parent: comp.webcamSlot
                anchors.fill: parent
                active: Studio.capVideoPlayback && editorWindow.webcamUrl !== ""
                        && editorProject && editorProject.style.webcamEnabled
                sourceComponent: PreviewVideo { source: editorWindow.webcamUrl }
            }

            // Poster-frame fallback (no QtMultimedia). posterSource is filled by
            // C++ once ffmpeg extracts the still.
            Image {
                parent: comp.videoSlot
                anchors.fill: parent
                visible: !Studio.capVideoPlayback
                source: editorWindow.posterSource
                // Stretch to fill the video region (same rationale as PreviewVideo):
                // the region carries the camera-viewport aspect and the composition
                // crops from there, so a plain stretch is undistorted in both modes.
                fillMode: Image.Stretch
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
                // Disabled while a keyframe is selected so the on-canvas crop editor
                // (beneath, in the video slot) receives the drags instead. Deselect
                // to add another zoom by clicking the canvas.
                enabled: editorProject !== null && editorWindow.selectedKeyframe < 0
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

    // Keyboard cheat sheet (themed Popup — no kit UShortcutsHelp component exists).
    Popup {
        id: shortcutsPopup
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: Theme.spacingXL
        Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.45) }
        background: Rectangle {
            radius: Theme.radiusL
            color: Theme.surface
            border.width: 1
            border.color: Theme.divider
        }
        readonly property var _rows: [
            { k: "Space",        a: qsTr("Play / pause") },
            { k: "← / →",        a: qsTr("Step one frame") },
            { k: "Shift + ← / →", a: qsTr("Jump one second") },
            { k: "Home / End",   a: qsTr("Go to trim start / end") },
            { k: "K",            a: qsTr("Add zoom at playhead") },
            { k: "Delete",       a: qsTr("Delete selected keyframe") },
            { k: "Esc",          a: qsTr("Deselect keyframe") },
            { k: "Ctrl + S",     a: qsTr("Save project") },
            { k: "Ctrl + E",     a: qsTr("Export…") },
            { k: "?",            a: qsTr("Show this help") }
        ]
        contentItem: Column {
            spacing: Theme.spacingM
            Text {
                text: qsTr("Keyboard shortcuts")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontL
                font.weight: Font.DemiBold
            }
            Column {
                spacing: Theme.spacingS
                Repeater {
                    model: shortcutsPopup._rows
                    Row {
                        required property var modelData
                        spacing: Theme.spacingL
                        Text {
                            width: 120
                            text: modelData.k
                            color: Theme.accent
                            font.pixelSize: Theme.fontS
                            font.weight: Font.DemiBold
                        }
                        Text {
                            text: modelData.a
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontS
                        }
                    }
                }
            }
            UButton {
                anchors.right: parent.right
                text: qsTr("Close")
                variant: "filled"
                compact: true
                onClicked: shortcutsPopup.close()
            }
        }
    }

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
