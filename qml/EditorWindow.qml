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
                top: toolbar.bottom; left: parent.left; right: parent.right; bottom: transport.top
                topMargin: Theme.spacingM
                bottomMargin: Theme.spacingM
            }

            CompositionRoot {
                id: comp
                anchors.fill: parent
                anchors.margins: Theme.spacingL
                styleModel: editorProject ? editorProject.style : null
                videoSize: editorProject ? editorProject.videoSize : Qt.size(1920, 1080)
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
                onClicked: if (videoLoader.item) videoLoader.item.togglePlay()
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
                onMoved: (v) => { if (videoLoader.item) videoLoader.item.seek(v) }
            }

            Text {
                id: timeLabel
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: editorWindow.mmss(editorWindow.curPos) + " / " + editorWindow.mmss(editorWindow.curDur)
                color: Theme.textSecondary
                font.pixelSize: Theme.fontS
            }
        }
    }

    // Export modal (owns its own ExportController + offscreen decode/render).
    ExportDialog {
        id: exportDialog
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
