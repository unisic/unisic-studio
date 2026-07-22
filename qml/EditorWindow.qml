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
    // encodeURI leaves '#' and '?' alone — in a file URL they become fragment/
    // query markers and the player then loads a truncated path. Escape them.
    function fileUrl(p) {
        return p === "" ? ""
             : "file://" + encodeURI(p).replace(/#/g, "%23").replace(/\?/g, "%3F")
    }
    readonly property string videoUrl: fileUrl(videoPath)
    // Optional webcam sidecar (composited as a corner overlay in the preview).
    readonly property string webcamUrl: (editorProject && editorProject.hasWebcam)
        ? fileUrl(editorProject.webcamResolved) : ""

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
    // Editing shortcuts stand down while any modal popup is up — a window-scope
    // Shortcut still fires behind an open Popup (Space would toggle playback
    // under the export dialog, Esc-deselect would preempt CloseOnEscape).
    readonly property bool modalOpen: exportDialog.opened || shortcutsPopup.opened
                                      || discardDlg.opened || abortExportDlg.opened

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

    // The source region the composition shows while editing a keyframe in fill
    // mode: the keyframe rect zoomed out 2x (clamped to the largest output-aspect
    // crop), so the box has context around it WITHOUT the letterbox jump to
    // source aspect (which is not what the export renders). Captured once per
    // selection — deliberately NOT live-bound to the keyframe rect, or the view
    // would chase the box while dragging it.
    property rect editViewRect: Qt.rect(0, 0, 1, 1)
    function computeEditView() {
        if (selectedKeyframe < 0 || !editorProject || !editorProject.zoom)
            return Qt.rect(0, 0, 1, 1)
        var st = editorProject.style
        if (!st || st.fillMode !== "fill")
            return Qt.rect(0, 0, 1, 1)          // fit mode: whole source, as before
        var out = outputAspect, src = srcAspect
        if (Math.abs(out - src) < 1e-4)
            return Qt.rect(0, 0, 1, 1)          // same aspect: full frame IS output-shaped
        // Largest output-aspect crop of the source (normalized source coords).
        var maxW = out >= src ? 1.0 : out / src
        var maxH = out >= src ? src / out : 1.0
        var m = editorProject.zoom.keyframeAt(selectedKeyframe)
        if (!m || m.w === undefined || m.w <= 0 || m.w >= 0.98)
            return Qt.rect((1 - maxW) / 2, (1 - maxH) / 2, maxW, maxH) // reset kf: base crop
        var w = Math.min(m.w * 2, maxW)
        var h = w * src / out                    // output-aspect: nh = nw * src/out
        var x = Math.max(0, Math.min(m.x + m.w / 2 - w / 2, 1 - w))
        var y = Math.max(0, Math.min(m.y + m.h / 2 - h / 2, 1 - h))
        return Qt.rect(x, y, w, h)
    }

    // Selecting a keyframe seeks to its instant so the on-canvas crop editor shows
    // the source frame at that moment, and captures the edit view for this
    // selection (see computeEditView).
    onSelectedKeyframeChanged: {
        editViewRect = computeEditView()
        if (selectedKeyframe >= 0 && editorProject && editorProject.zoom) {
            var m = editorProject.zoom.keyframeAt(selectedKeyframe)
            if (m && m.tMs !== undefined)
                seekTo(m.tMs)
        }
    }

    // One seek entry point: with live playback it moves the player (whose
    // position feeds preview.sync); without it, it snaps the preview clock.
    function seekTo(ms) {
        var targetMs = Math.max(0, ms)
        var playing = hasPlayback && videoLoader.item.isPlaying
        if (playing && (targetMs < trimInMs || targetMs >= trimOutMs)) {
            videoLoader.item.pause()
            if (webcamLoader.item) webcamLoader.item.pause()
            playing = false
        }
        if (typeof preview !== "undefined" && preview) {
            if (hasPlayback) preview.sync(targetMs, playing)
            else preview.snap(targetMs)
        }
        if (hasPlayback) videoLoader.item.seek(targetMs)
        if (webcamLoader.item) webcamLoader.item.seek(targetMs) // keep the overlay aligned
    }

    // Start playback from the trim-in point when the head is outside the trimmed
    // range, then toggle. Keeps the preview confined to the exported span.
    function playPause() {
        // Playing means "show me the result" — leave keyframe-edit mode first so
        // the composition returns to the real camera instead of the edit view.
        if (selectedKeyframe >= 0)
            selectedKeyframe = -1
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

    // Per-rendered-frame drive of the preview clock: FrameAnimation fires in the
    // scene graph's animation phase, so the camera/cursor advance exactly once
    // per displayed frame at ANY refresh rate (a fixed 16 ms timer beat against
    // vsync and juddered). Runs only while the preview is playing.
    FrameAnimation {
        running: (typeof preview !== "undefined" && preview) ? preview.playing : false
        onTriggered: preview.frameTick()
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
    Connections {
        target: (typeof preview !== "undefined") ? preview : null
        ignoreUnknownSignals: true
        function onPlaybackRangeEnded() {
            if (videoLoader.item) {
                videoLoader.item.pause()
                videoLoader.item.seek(editorWindow.trimOutMs)
            }
            if (webcamLoader.item) {
                webcamLoader.item.pause()
                webcamLoader.item.seek(editorWindow.trimOutMs)
            }
        }
    }

    // Keyframe row indices are unstable across add/remove, so a plain count change
    // drops the selection. A regenerate (replaceAutoKeyframes) is a full model
    // reset, but Manual/locked keyframes SURVIVE it — so we remember the selected
    // keyframe's time before the reset and re-select it afterwards if it lived.
    // Without this, editing a zoom then nudging a slider deselects your keyframe.
    property real _reselectTMs: -1     // selected kf time captured before a reset
    property bool _resetInFlight: false
    Connections {
        target: editorProject ? editorProject.zoom : null
        ignoreUnknownSignals: true
        function onModelAboutToBeReset() {
            var m = (editorWindow.selectedKeyframe >= 0 && editorProject && editorProject.zoom)
                    ? editorProject.zoom.keyframeAt(editorWindow.selectedKeyframe) : null
            editorWindow._reselectTMs = (m && m.tMs !== undefined) ? m.tMs : -1
            editorWindow._resetInFlight = true
        }
        function onModelReset() {
            var restore = -1
            if (editorWindow._reselectTMs >= 0 && editorProject && editorProject.zoom) {
                for (var i = 0; i < editorProject.zoom.count; ++i) {
                    var m = editorProject.zoom.keyframeAt(i)
                    if (m && m.tMs !== undefined
                            && Math.abs(m.tMs - editorWindow._reselectTMs) < 0.5) {
                        restore = i
                        break
                    }
                }
            }
            editorWindow._reselectTMs = -1
            editorWindow.selectedKeyframe = restore
            // The conditional countChanged that replaceAutoKeyframes may emit fires
            // synchronously right after this; keep the guard up until that batch
            // drains so it doesn't clobber the restored selection.
            Qt.callLater(function() { editorWindow._resetInFlight = false })
        }
        function onCountChanged() {
            if (editorWindow._resetInFlight) return   // post-reset: selection already set
            editorWindow.selectedKeyframe = -1        // genuine add/remove: indices shifted
        }
    }
    Connections {
        target: editorProject
        ignoreUnknownSignals: true
        function onTrimChanged() {
            if (videoLoader.item && videoLoader.item.isPlaying
                    && videoLoader.item.positionMs < editorWindow.trimInMs)
                editorWindow.seekTo(editorWindow.trimInMs)
        }
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
        enabled: !editorWindow.typingText && !editorWindow.modalOpen
        onActivated: editorWindow.playPause()
    }
    Shortcut {
        sequence: "Left"
        enabled: !editorWindow.typingText && !editorWindow.modalOpen
        onActivated: editorWindow.stepBy(-editorWindow.frameStepMs)
    }
    Shortcut {
        sequence: "Right"
        enabled: !editorWindow.typingText && !editorWindow.modalOpen
        onActivated: editorWindow.stepBy(editorWindow.frameStepMs)
    }
    Shortcut {
        sequence: "Shift+Left"
        enabled: !editorWindow.typingText && !editorWindow.modalOpen
        onActivated: editorWindow.stepBy(-1000)
    }
    Shortcut {
        sequence: "Shift+Right"
        enabled: !editorWindow.typingText && !editorWindow.modalOpen
        onActivated: editorWindow.stepBy(1000)
    }
    Shortcut {
        sequence: "Home"
        enabled: !editorWindow.typingText && !editorWindow.modalOpen
        onActivated: editorWindow.seekTo(editorWindow.trimInMs)
    }
    Shortcut {
        sequence: "End"
        enabled: !editorWindow.typingText && !editorWindow.modalOpen
        onActivated: editorWindow.seekTo(editorWindow.trimOutMs)
    }
    Shortcut {
        sequences: [StandardKey.Delete, StandardKey.Backspace]
        enabled: !editorWindow.typingText && !editorWindow.modalOpen && editorWindow.selectedKeyframe >= 0
        onActivated: editorWindow.deleteSelectedKeyframe()
    }
    Shortcut {
        sequence: "K"
        enabled: !editorWindow.typingText && !editorWindow.modalOpen && editorProject !== null
        onActivated: editorWindow.addZoomAtHead()
    }
    Shortcut { // Esc deselects the current keyframe (dialogs handle their own Esc).
<<<<<<< HEAD
        // Explicit "Escape" alongside StandardKey.Cancel: the platform theme may
        // leave Cancel unbound, and this is the primary way OUT of edit mode.
        sequences: [StandardKey.Cancel, "Escape"]
        enabled: editorWindow.selectedKeyframe >= 0
=======
        sequence: StandardKey.Cancel
        enabled: !editorWindow.modalOpen && editorWindow.selectedKeyframe >= 0
>>>>>>> 14d89856a8754caa94ca67cdbe9fa6f8da48f97e
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
        // Closing tears down the export dialog's ExportController — a running
        // export would be silently killed and its partial output deleted.
        if (exportDialog.running && !confirmedClose) {
            close.accepted = false
            abortExportDlg.open()
            return
        }
        if (editorProject && editorProject.dirty && !confirmedClose) {
            close.accepted = false
            discardDlg.open()
        }
    }

    // ---- Custom title bar (frameless) — shared with StudioMain -------------
    // chromeTop is the bar height; downstream panes anchor to titleBar.bottom.
    StudioTitleBar {
        id: titleBar
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: editorWindow.chromeTop
        window: editorWindow
    }

    // Native edge/corner resizing for the frameless window (min-size aware).
    StudioResizeGrips { window: editorWindow }

    // Inspector width is user-draggable (see inspectorGrip) and persisted; the
    // collapse toggle in the toolbar hides it for a distraction-free canvas.
    readonly property int _inspectorMinW: 260
    readonly property int _inspectorMaxW: 560
    property int inspectorWidth: Math.max(_inspectorMinW,
                                          Math.min(_inspectorMaxW, Studio.settings.editorInspectorWidth))
    onInspectorWidthChanged: Studio.settings.editorInspectorWidth = inspectorWidth
    property bool inspectorCollapsed: false

    // ---- Right: style inspector --------------------------------------------
    InspectorPanel {
        id: inspector
        width: editorWindow.inspectorCollapsed ? 0 : editorWindow.inspectorWidth
        clip: true
        visible: width > 0
        anchors { top: titleBar.bottom; right: parent.right; bottom: parent.bottom }
        styleModel: editorProject ? editorProject.style : null
        selectedKeyframe: editorWindow.selectedKeyframe
        onDeselect: editorWindow.selectedKeyframe = -1
        Behavior on width { NumberAnimation { duration: Theme.animFast } }
    }

    // Drag handle between canvas and inspector — thin, brightens on hover/drag.
    Rectangle {
        id: inspectorGrip
        visible: !editorWindow.inspectorCollapsed
        width: 4
        anchors { top: titleBar.bottom; bottom: parent.bottom; right: inspector.left }
        color: (gripArea.containsMouse || gripArea.pressed) ? Theme.accent : Theme.border
        opacity: (gripArea.containsMouse || gripArea.pressed) ? 0.9 : 0.35
        Behavior on opacity { NumberAnimation { duration: Theme.animFast } }
        MouseArea {
            id: gripArea
            anchors.fill: parent
            anchors.leftMargin: -4   // fatter hit target than the visible line
            anchors.rightMargin: -4
            hoverEnabled: true
            cursorShape: Qt.SplitHCursor
            property real pressX: 0
            property int pressWidth: 0
            onPressed: (m) => {
                pressX = mapToItem(editorWindow.contentItem, m.x, m.y).x
                pressWidth = editorWindow.inspectorWidth
            }
            onPositionChanged: (m) => {
                if (!pressed) return
                var gx = mapToItem(editorWindow.contentItem, m.x, m.y).x
                editorWindow.inspectorWidth = Math.max(editorWindow._inspectorMinW,
                    Math.min(editorWindow._inspectorMaxW, pressWidth + (pressX - gx)))
            }
        }
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

            Row {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spacingS

                UIconButton {
                    // Re-show the projects/launcher window. Closing it earlier only
                    // hides it (it is the engine root), so this is the way back — no
                    // more orphaned session.
                    iconName: "go-home"
                    iconSize: 14
                    tooltip: qsTr("Projects")
                    anchors.verticalCenter: parent.verticalCenter
                    onClicked: Studio.showMainWindow()
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: editorWindow.projectName
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontL
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, toolbar.width * 0.32)
                }
            }

            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spacingM

                UIconButton {
                    // Collapse the inspector for a distraction-free canvas.
                    iconName: editorWindow.inspectorCollapsed ? "arrow-left" : "arrow-right"
                    iconSize: 14
                    tooltip: editorWindow.inspectorCollapsed ? qsTr("Show inspector")
                                                             : qsTr("Hide inspector")
                    anchors.verticalCenter: parent.verticalCenter
                    onClicked: editorWindow.inspectorCollapsed = !editorWindow.inspectorCollapsed
                }

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
                // Feeds the "desktopBlur" background (poster of the first frame).
                posterSource: editorWindow.posterSource
                // A webcam feed is present only when one was recorded, the module
                // is available, and the style enables it.
                webcamHasFeed: webcamLoader.item !== null
                // While a keyframe is selected: fit mode shows the whole source
                // frame; fill mode keeps the output-aspect card (what the export
                // renders) and shows editView — a zoomed-out context region
                // around the keyframe. No more letterbox jump on selection.
                editRect: editorWindow.selectedKeyframe >= 0
                editView: editorWindow.editViewRect
            }

            // On-canvas zoom-target editor for the selected keyframe. Parented into
            // the composition's editorSlot — OUTSIDE the camera transform — and maps
            // source coords through the same view rect the composition displays.
            // Editor-only — the export renderer instantiates CompositionRoot
            // without it.
            Loader {
                parent: comp.editorSlot
                anchors.fill: parent
                z: 100
                active: editorProject !== null && editorWindow.selectedKeyframe >= 0
                sourceComponent: ZoomRectEditor {
                    project: editorProject
                    index: editorWindow.selectedKeyframe
                    sourceAspect: editorWindow.srcAspect
<<<<<<< HEAD
                    outputAspect: editorWindow.outputAspect
                    viewRect: comp._effZoom
=======
                    // Rects render stretched into the video region: output-aspect
                    // in fill mode, source-aspect in fit (letterbox) mode.
                    outputAspect: (editorProject && editorProject.style
                                   && editorProject.style.fillMode === "fit")
                                  ? editorWindow.srcAspect : editorWindow.outputAspect
>>>>>>> 14d89856a8754caa94ca67cdbe9fa6f8da48f97e
                    onDeselectRequested: editorWindow.selectedKeyframe = -1
                }
            }

            // Explicit way OUT of keyframe-edit mode, floating over the canvas.
            // (Esc, Space and clicking outside the box also leave it — this one
            // is the discoverable path.)
            UButton {
                visible: editorWindow.selectedKeyframe >= 0
                z: 200
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: Theme.spacingM
                variant: "filled"
                compact: true
                text: qsTr("Done (Esc)")
                onClicked: editorWindow.selectedKeyframe = -1
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
                value: editorWindow.headMs
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
                text: editorWindow.mmss(editorWindow.headMs) + " / " + editorWindow.mmss(editorWindow.curDur)
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

    UConfirmDialog {
        id: abortExportDlg
        title: qsTr("Stop the export?")
        text: qsTr("An export is still running. Closing this window will cancel it "
                   + "and discard the partial file.")
        confirmText: qsTr("Stop export")
        cancelText: qsTr("Keep exporting")
        destructive: true
        onAccepted: { editorWindow.confirmedClose = true; editorWindow.close() }
    }
}
