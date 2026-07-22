import QtQuick
import QtQuick.Controls
import Unisic.Kit
import UnisicStudio

// Modal export dialog (kit Popup on Overlay.overlay with a dimming scrim). One
// clean column: a segmented Format row, a Quality-preset row (Small/Balanced/High)
// with an Advanced disclosure hiding the raw resolution/fps/quality fields, a
// folder + editable-filename destination, and a full-width Export button. During a
// run the form is replaced in place by a progress view (percent + ETA + Cancel);
// on completion by a Done view (Open file / Show in folder / Close). GIF selection
// caps fps at 30, trims the fps choices, and shows a size hint. It owns its own
// ExportController, so it decodes independently of the editor's live preview. Every
// control is a kit component, every colour a Theme token, every string qsTr()'d.
Popup {
    id: root

    // The StudioProject* to export (set by EditorWindow).
    property var project: null

    // Destination is split into an editable folder + base filename; the extension
    // always tracks the selected format (applyDest recomposes exporter.outputPath).
    property string folder: ""
    property string fileBase: ""
    // Advanced (resolution / fps / custom size / raw quality) collapsed by default.
    property bool advancedOpen: false

    readonly property bool running: exporter.state === ExportController.Running
    readonly property bool done: exporter.state === ExportController.Done
    // The form is shown whenever we are not actively exporting or finished.
    readonly property bool showForm: !running && !done
    readonly property bool isGif: exporter.format === "gif"

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

    ExportController { id: exporter }

    // Index maps between the model's string enums and the combo rows.
    readonly property var _formats: ["mp4", "webm", "gif"]
    readonly property var _resolutions: ["source", "1080p", "720p", "custom"]

    // Quality presets: each maps to a (quality, resolution-cap) pair. The chip is
    // "checked" purely by matching the current controller state, so the Advanced
    // fields and the presets stay consistent without extra bookkeeping.
    readonly property var _presets: [
        { key: "small",    label: qsTr("Small"),    quality: 35, resolution: "720p" },
        { key: "balanced", label: qsTr("Balanced"), quality: 70, resolution: "1080p" },
        { key: "high",     label: qsTr("High"),     quality: 92, resolution: "source" }
    ]

    // Exported clip length (mirrors the editor's trim range) — feeds the GIF hint.
    readonly property int clipMs: {
        if (!project) return 0
        var out = project.trimOutMs > project.trimInMs ? project.trimOutMs : project.durationMs
        return Math.max(0, out - project.trimInMs)
    }

    function mmss(ms) {
        if (isNaN(ms) || ms < 0) ms = 0
        var s = Math.round(ms / 1000), m = Math.floor(s / 60)
        s = s % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }
    function dateStamp() { return Qt.formatDate(new Date(), "yyyyMMdd") }

    // Recompose the destination path from folder + base + the format's extension.
    function applyDest() {
        if (folder === "" || fileBase === "") { exporter.outputPath = ""; return }
        var b = fileBase
        var ext = exporter.extension
        if (b.toLowerCase().endsWith("." + ext)) b = b.substring(0, b.length - ext.length - 1)
        exporter.outputPath = folder + "/" + b + "." + ext
    }
    onFolderChanged: applyDest()
    onFileBaseChanged: applyDest()

    function applyPreset(p) {
        exporter.quality = p.quality
        exporter.resolution = p.resolution
    }
    function presetChecked(p) {
        return exporter.quality === p.quality && exporter.resolution === p.resolution
    }
    // True when the current (quality, resolution) pair matches no preset — the user
    // drove the Advanced fields directly. The chips stay honestly unlit (a lit chip
    // whose settings the encoder ignores would lie); this drives a quiet "Custom"
    // hint instead, so the row doesn't read as "nothing selected".
    readonly property bool customPreset: {
        for (var i = 0; i < _presets.length; ++i)
            if (presetChecked(_presets[i]))
                return false
        return true
    }

    onOpened: {
        // Start fresh when reopened after a previous run.
        if (!running)
            exporter.reset()
        if (project) {
            if (folder === "")
                folder = exporter.suggestedDir(project)
            if (fileBase === "")
                fileBase = exporter.suggestedBaseName(project) + "-" + dateStamp()
        }
        applyDest()
    }

    // Keep the destination extension in lockstep with the format.
    Connections {
        target: exporter
        function onFormatChanged() { root.applyDest() }
    }

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
            text: root.done ? qsTr("Export complete")
                 : root.running ? qsTr("Exporting…")
                 : qsTr("Export video")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontL
            font.weight: Font.DemiBold
        }

        // ================= Settings form ==================================
        Column {
            width: parent.width
            spacing: Theme.spacingL
            visible: root.showForm

            // ---- Format (segmented) ----
            Field {
                label: qsTr("Format")
                Row {
                    width: parent.width
                    spacing: Theme.spacingS
                    Repeater {
                        model: [
                            { id: "mp4",  label: qsTr("MP4") },
                            { id: "webm", label: qsTr("WebM") },
                            { id: "gif",  label: qsTr("GIF") }
                        ]
                        UFilterChip {
                            required property var modelData
                            text: modelData.label
                            checked: exporter.format === modelData.id
                            onClicked: exporter.format = modelData.id
                        }
                    }
                }
            }

            // ---- Quality preset (segmented) ----
            Field {
                label: qsTr("Quality")
                Column {
                    width: parent.width
                    spacing: Theme.spacingS
                    Row {
                        width: parent.width
                        spacing: Theme.spacingS
                        Repeater {
                            model: root._presets
                            UFilterChip {
                                required property var modelData
                                text: modelData.label
                                checked: root.presetChecked(modelData)
                                onClicked: root.applyPreset(modelData)
                            }
                        }
                    }
                    // No chip matches: say so rather than showing a blank row.
                    Text {
                        width: parent.width
                        visible: root.customPreset
                        text: qsTr("Custom — set in Advanced below.")
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontS
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // ---- GIF size hint ----
            Text {
                width: parent.width
                visible: root.isGif
                text: qsTr("GIF · no audio · capped at 30 fps · large files, best for short clips (%1).")
                      .arg(root.mmss(root.clipMs))
                color: Theme.textTertiary
                font.pixelSize: Theme.fontS
                wrapMode: Text.WordWrap
            }

            // ---- Advanced disclosure ----
            Column {
                width: parent.width
                spacing: Theme.spacingM

                Row {
                    id: advHeader
                    spacing: Theme.spacingS
                    UIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        name: root.advancedOpen ? "arrow-down" : "arrow-right"
                        size: 13
                        color: Theme.textSecondary
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Advanced")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontS
                        font.weight: Font.DemiBold
                    }
                    // Whole header is the toggle.
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.advancedOpen = !root.advancedOpen
                    }
                }

                Column {
                    width: parent.width
                    spacing: Theme.spacingM
                    visible: root.advancedOpen

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

                    // Frame rate. GIF drops the 60 fps choice (capped at 30).
                    Field {
                        label: qsTr("Frame rate")
                        UComboBox {
                            width: parent.width
                            readonly property var ids: root.isGif ? ["source", "30"]
                                                                  : ["source", "30", "60"]
                            model: root.isGif ? [qsTr("Source"), qsTr("30 fps")]
                                              : [qsTr("Source"), qsTr("30 fps"), qsTr("60 fps")]
                            currentIndex: Math.max(0, ids.indexOf(exporter.fpsMode))
                            onActivated: (i) => exporter.fpsMode = ids[i]
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
                }
            }

            // ---- Destination (folder + filename) ----
            Field {
                label: qsTr("Save to")
                Column {
                    width: parent.width
                    spacing: Theme.spacingS
                    Row {
                        width: parent.width
                        spacing: Theme.spacingS
                        Text {
                            width: parent.width - changeBtn.width - parent.spacing
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.folder === "" ? qsTr("No folder chosen") : root.folder
                            color: root.folder === "" ? Theme.textTertiary : Theme.textSecondary
                            font.pixelSize: Theme.fontS
                            elide: Text.ElideMiddle
                        }
                        UButton {
                            id: changeBtn
                            text: qsTr("Change…")
                            variant: "tonal"
                            compact: true
                            onClicked: {
                                const d = Studio.pickExportFolder(root.folder)
                                if (d !== "") root.folder = d
                            }
                        }
                    }
                    Row {
                        width: parent.width
                        spacing: Theme.spacingS
                        UTextField {
                            id: nameField
                            width: parent.width - extLabel.width - parent.spacing
                            placeholder: qsTr("File name")
                            text: root.fileBase
                            onEdited: (t) => root.fileBase = t
                        }
                        Text {
                            id: extLabel
                            anchors.verticalCenter: parent.verticalCenter
                            text: "." + exporter.extension
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontM
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

        // ================= Progress / Done ================================
        Column {
            width: parent.width
            spacing: Theme.spacingM
            visible: root.running || root.done

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
                    text: root.done ? qsTr("Done")
                                    : Math.round(exporter.progress * 100) + "%"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontS
                }
                Text {
                    anchors.right: parent.right
                    // ETA while running (once we have a rate); frame count otherwise.
                    text: root.running && exporter.etaMs > 0
                          ? qsTr("about %1 left").arg(root.mmss(exporter.etaMs))
                          : (exporter.totalFrames > 0 && root.running
                             ? qsTr("%1 / %2 frames").arg(exporter.framesDone).arg(exporter.totalFrames)
                             : "")
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontS
                }
            }

            // Done: where the file landed.
            Text {
                width: parent.width
                visible: root.done
                text: exporter.outputPath
                color: Theme.textTertiary
                font.pixelSize: Theme.fontS
                elide: Text.ElideMiddle
            }
        }

        // ================= Actions ========================================
        // Form: one full-width accent Export, a quiet Cancel below.
        Column {
            width: parent.width
            spacing: Theme.spacingS
            visible: root.showForm
            UButton {
                width: parent.width
                text: qsTr("Export")
                variant: "filled"
                enabled: exporter.outputPath !== "" && root.project !== null
                onClicked: if (root.project) exporter.start(root.project)
            }
            UButton {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Cancel")
                variant: "ghost"
                compact: true
                onClicked: root.close()
            }
        }

        // Running: cancel.
        UButton {
            anchors.right: parent.right
            visible: root.running
            text: qsTr("Cancel")
            variant: "danger"
            compact: true
            onClicked: exporter.cancel()
        }

        // Done: open / reveal / close.
        Row {
            anchors.right: parent.right
            spacing: Theme.spacingS
            visible: root.done
            UButton {
                text: qsTr("Open file")
                variant: "filled"
                compact: true
                onClicked: Studio.openFile(exporter.outputPath)
            }
            UButton {
                text: qsTr("Show in folder")
                variant: "tonal"
                compact: true
                onClicked: Studio.revealInFolder(exporter.outputPath)
            }
            UButton {
                text: qsTr("Close")
                variant: "ghost"
                compact: true
                onClicked: root.close()
            }
        }
    }
}
