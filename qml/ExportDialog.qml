import QtQuick
import QtQuick.Controls
import Unisic.Kit
import UnisicStudio

// Modal export dialog (kit UConfirmDialog styling: a Popup centered on
// Overlay.overlay with a dimming scrim). Two faces driven by ExportController's
// state: a settings form (format / resolution / fps / quality / destination) and
// a progress view (bar + percent + frame count + cancel). On Done it offers
// "Reveal in folder" and Close. Every control is a kit component; every colour a
// Theme token; every string qsTr()'d. It owns its ExportController, so it decodes
// independently of the editor's live preview.
Popup {
    id: root

    // The StudioProject* to export (set by EditorWindow).
    property var project: null

    readonly property bool running: exporter.state === ExportController.Running
    readonly property bool done: exporter.state === ExportController.Done
    // The form is shown whenever we are not actively exporting or finished.
    readonly property bool showForm: !running && !done

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    // No accidental dismissal mid-export.
    closePolicy: running ? Popup.NoAutoClose : (Popup.CloseOnEscape | Popup.CloseOnPressOutside)
    width: Math.min(480, parent ? parent.width - 2 * Theme.spacingXL : 480)
    padding: Theme.spacingXL

    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.45) }
    background: Rectangle {
        radius: Theme.radiusL
        color: Theme.surface
        border.width: 1
        border.color: Theme.divider
    }

    onOpened: {
        // Start fresh when reopened after a previous run; seed the destination.
        if (!running)
            exporter.reset()
        if (project && exporter.outputPath === "")
            exporter.outputPath = exporter.suggestedOutputPath(project)
    }

    ExportController { id: exporter }

    // Index maps between the model's string enums and the combo rows.
    readonly property var _formats: ["mp4", "webm"]
    readonly property var _resolutions: ["source", "1080p", "720p", "custom"]
    readonly property var _fpsModes: ["source", "30", "60"]

    // A labelled column wrapper for a control.
    component Field: Column {
        property string label: ""
        width: parent ? parent.width : 0
        spacing: 4
        property alias content: holder.data
        Text {
            text: parent.label
            color: Theme.textSecondary
            font.pixelSize: Theme.fontS
        }
        Item {
            id: holder
            width: parent.width
            height: childrenRect.height
        }
    }

    contentItem: Column {
        spacing: Theme.spacingL

        Text {
            width: parent.width
            text: qsTr("Export video")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontL
            font.weight: Font.DemiBold
        }

        // ---- Settings form -------------------------------------------------
        Column {
            width: parent.width
            spacing: Theme.spacingM
            visible: root.showForm

            Field {
                label: qsTr("Format")
                UComboBox {
                    width: parent.width
                    model: [qsTr("MP4 (H.264)"), qsTr("WebM (VP9)")]
                    currentIndex: Math.max(0, root._formats.indexOf(exporter.format))
                    onActivated: (i) => exporter.format = root._formats[i]
                }
            }

            Field {
                label: qsTr("Resolution")
                UComboBox {
                    width: parent.width
                    model: [qsTr("Source"), "1080p", "720p", qsTr("Custom")]
                    currentIndex: Math.max(0, root._resolutions.indexOf(exporter.resolution))
                    onActivated: (i) => exporter.resolution = root._resolutions[i]
                }
            }

            // Custom W/H only when the "custom" preset is chosen.
            Row {
                width: parent.width
                spacing: Theme.spacingM
                visible: exporter.resolution === "custom"
                Field {
                    width: (parent.width - parent.spacing) / 2
                    label: qsTr("Width")
                    UTextField {
                        width: parent.width
                        text: String(exporter.customWidth)
                        validator: IntValidator { bottom: 2; top: 7680 }
                        onEdited: (t) => { if (t.length > 0) exporter.customWidth = parseInt(t) }
                    }
                }
                Field {
                    width: (parent.width - parent.spacing) / 2
                    label: qsTr("Height")
                    UTextField {
                        width: parent.width
                        text: String(exporter.customHeight)
                        validator: IntValidator { bottom: 2; top: 4320 }
                        onEdited: (t) => { if (t.length > 0) exporter.customHeight = parseInt(t) }
                    }
                }
            }

            Field {
                label: qsTr("Frame rate")
                UComboBox {
                    width: parent.width
                    model: [qsTr("Source"), qsTr("30 fps"), qsTr("60 fps")]
                    currentIndex: Math.max(0, root._fpsModes.indexOf(exporter.fpsMode))
                    onActivated: (i) => exporter.fpsMode = root._fpsModes[i]
                }
            }

            Field {
                label: qsTr("Quality: %1%").arg(exporter.quality)
                USlider {
                    width: parent.width
                    from: 0
                    to: 100
                    stepSize: 1
                    value: exporter.quality
                    onMoved: (v) => exporter.quality = Math.round(v)
                }
            }

            Field {
                label: qsTr("Destination")
                Row {
                    width: parent.width
                    spacing: Theme.spacingS
                    UTextField {
                        id: destField
                        width: parent.width - browseBtn.width - parent.spacing
                        readOnly: true
                        placeholder: qsTr("Choose a destination file…")
                        text: exporter.outputPath
                    }
                    UButton {
                        id: browseBtn
                        text: qsTr("Browse…")
                        variant: "tonal"
                        onClicked: {
                            const p = Studio.pickExportOutput(exporter.extension, exporter.outputPath)
                            if (p !== "")
                                exporter.outputPath = p
                        }
                    }
                }
            }

            // Error from a previous attempt.
            Text {
                width: parent.width
                visible: exporter.state === ExportController.Error && exporter.errorString !== ""
                text: exporter.errorString
                color: Theme.danger
                font.pixelSize: Theme.fontS
                wrapMode: Text.WordWrap
            }
        }

        // ---- Progress view -------------------------------------------------
        Column {
            width: parent.width
            spacing: Theme.spacingM
            visible: root.running || root.done

            Text {
                width: parent.width
                text: root.done ? qsTr("Export complete") : qsTr("Exporting…")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontM
                font.weight: Font.DemiBold
            }

            // Themed progress bar (kit primitives: a track with an accent fill).
            Rectangle {
                width: parent.width
                height: 8
                radius: 4
                color: Theme.surfaceHi
                Rectangle {
                    width: parent.width * Math.max(0, Math.min(1, exporter.progress))
                    height: parent.height
                    radius: 4
                    color: Theme.accent
                    Behavior on width { NumberAnimation { duration: Theme.animFast } }
                }
            }

            Item {
                width: parent.width
                height: percentText.implicitHeight
                Text {
                    id: percentText
                    anchors.left: parent.left
                    text: Math.round(exporter.progress * 100) + "%"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontS
                }
                Text {
                    anchors.right: parent.right
                    text: exporter.totalFrames > 0
                          ? qsTr("%1 / %2 frames").arg(exporter.framesDone).arg(exporter.totalFrames)
                          : ""
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontS
                }
            }
        }

        // ---- Action buttons ------------------------------------------------
        Row {
            anchors.right: parent.right
            spacing: Theme.spacingS

            // Form actions.
            UButton {
                visible: root.showForm
                text: qsTr("Cancel")
                variant: "ghost"
                compact: true
                onClicked: root.close()
            }
            UButton {
                visible: root.showForm
                text: qsTr("Export")
                variant: "filled"
                compact: true
                enabled: exporter.outputPath !== ""
                onClicked: if (root.project) exporter.start(root.project)
            }

            // Running action.
            UButton {
                visible: root.running
                text: qsTr("Cancel")
                variant: "danger"
                compact: true
                onClicked: exporter.cancel()
            }

            // Done actions.
            UButton {
                visible: root.done
                text: qsTr("Reveal in folder")
                variant: "tonal"
                compact: true
                onClicked: Studio.revealInFolder(exporter.outputPath)
            }
            UButton {
                visible: root.done
                text: qsTr("Close")
                variant: "filled"
                compact: true
                onClicked: root.close()
            }
        }
    }
}
