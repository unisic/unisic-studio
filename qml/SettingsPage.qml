import QtQuick
import QtQuick.Controls
import Unisic.Kit

// Settings page shown in StudioMain's content area (sidebar → Settings). Left
// category rail + lazily-loaded panes, mirroring Unisic's SettingsPage at smaller
// scale: inline SettingRow / SectionTitle / ScrollPane components, the kit U*
// controls, the `ids`-array + indexOf combo idiom, and ThemeController.themeName
// as the single theme write-path. No in-app search in v1. Every string qsTr()'d,
// every color a Theme token (the theme-preview swatches read seed colors from the
// kit's theme data, never hardcoded here).
Item {
    id: page

    readonly property int cardWidth: Math.min(paneArea.width, 640)
    property int tab: 0

    // The Developer tab is appended only in dev builds (UNISIC_DEV_BUILD →
    // Studio.devBuild), same gating idiom as Unisic's Settings.
    readonly property var tabNames: Studio.devBuild
        ? [qsTr("Recording"), qsTr("Storage"), qsTr("Appearance"), qsTr("About"), qsTr("Developer")]
        : [qsTr("Recording"), qsTr("Storage"), qsTr("Appearance"), qsTr("About")]
    readonly property var tabIcons: Studio.devBuild
        ? ["media-record", "folder", "fill-color", "help-about", "applications-development"]
        : ["media-record", "folder", "fill-color", "help-about"]

    // All nine selectable themes, "system" first (it follows the desktop scheme).
    readonly property var themeIds: ["system", "unisic", "dark", "light",
                                     "catppuccin-mocha", "catppuccin-latte",
                                     "dracula", "nord", "gruvbox"]
    readonly property var themeNames: [qsTr("System"), "Unisic", qsTr("Dark"), qsTr("Light"),
                                       "Catppuccin Mocha", "Catppuccin Latte",
                                       "Dracula", "Nord", "Gruvbox"]

    // Mirrors InputPermission::Status (StudioApp.inputPermissionStatus is that int,
    // or -1 before the first probe).
    readonly property int permAvailable: 0
    readonly property int permNoPermission: 1
    readonly property int permNotBuilt: 2

    // CRF bounds for the quality slider (lower CRF = higher quality, so the slider
    // is inverted: quality = crfMin + crfMax - crf).
    readonly property int crfMin: 14
    readonly property int crfMax: 28

    // Three preview swatch colors for a theme id. The kit exposes no per-theme
    // seed API, so read them from the theme data directly: the live system palette
    // for "system", else the private _defs map (falls back to the active Theme
    // tokens if unavailable). Keeps zero palette hex in this file.
    function seedsFor(id) {
        if (id === "system")
            return [ThemeController.sysWindow, ThemeController.sysButton, ThemeController.sysAccent]
        var d = Theme._defs ? Theme._defs[id] : undefined
        if (!d)
            return [Theme.primary, Theme.secondary, Theme.accent]
        return [d.primary, d.secondary, d.accent]
    }

    function permText() {
        switch (Studio.inputPermissionStatus) {
        case page.permAvailable:    return qsTr("Input access available")
        case page.permNoPermission: return qsTr("No input permission")
        case page.permNotBuilt:     return qsTr("Input capture not built")
        default:                    return qsTr("Checking input access…")
        }
    }
    function permColor() {
        switch (Studio.inputPermissionStatus) {
        case page.permAvailable:    return Theme.success
        case page.permNoPermission: return Theme.danger
        default:                    return Theme.textTertiary
        }
    }

    // ---- inline building blocks (Unisic conventions, trimmed) ----

    component SectionTitle: Text {
        color: Theme.textPrimary
        font.pixelSize: Theme.fontL
        font.weight: Font.DemiBold
    }

    // A label (+ optional one-line help caption) on the left, a control in the
    // default `control` slot on the right. Grows for tall controls.
    component SettingRow: Item {
        id: settingRow
        property alias label: labelText.text
        property string help: ""
        default property alias control: slot.data
        width: parent ? parent.width : 0
        height: Math.max(48, textCol.height + 12, slot.height + 12)

        Column {
            id: textCol
            anchors.left: parent.left
            anchors.right: slot.left
            anchors.rightMargin: Theme.spacingM
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text {
                id: labelText
                width: parent.width
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.pixelSize: Theme.fontM
            }
            Text {
                visible: settingRow.help !== ""
                width: parent.width
                wrapMode: Text.WordWrap
                text: settingRow.help
                color: Theme.textTertiary
                font.pixelSize: Theme.fontS
            }
        }
        Item {
            id: slot
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: childrenRect.width
            height: childrenRect.height
        }
    }

    // A scrollable pane: give it cards as default content.
    component ScrollPane: Flickable {
        id: fl
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: paneCol.height + Theme.spacingXL
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        default property alias content: paneCol.data
        Column {
            id: paneCol
            width: fl.width
            spacing: Theme.spacingL
        }
        MiddleScroll { flickable: fl }
        WheelBoost { flickable: fl }
    }

    // One category-rail entry (icon + label, accent-tinted when active).
    component NavItem: Rectangle {
        id: nav
        property string iconName: ""
        property string label: ""
        property bool active: false
        signal clicked()
        width: parent ? parent.width : 170
        height: 38
        radius: Theme.radiusM
        color: active ? Theme.alpha(Theme.accent, 0.18)
             : navMouse.containsMouse ? Theme.alpha(Theme.accent, 0.10)
             : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.animFast } }
        Rectangle {
            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            width: 3; height: 18; radius: 1.5
            color: Theme.accent
            visible: nav.active
        }
        Row {
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10
            UIcon {
                name: nav.iconName
                size: 18
                color: nav.active ? Theme.accent : Theme.textSecondary
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: nav.label
                color: nav.active ? Theme.textPrimary : Theme.textSecondary
                font.pixelSize: Theme.fontM
                font.weight: nav.active ? Font.DemiBold : Font.Normal
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        MouseArea {
            id: navMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: nav.clicked()
        }
    }

    // ---- header ----
    Text {
        id: header
        anchors.top: parent.top
        anchors.topMargin: Theme.spacingXL
        anchors.left: parent.left
        anchors.leftMargin: Theme.spacingXL
        text: qsTr("Settings")
        color: Theme.textPrimary
        font.pixelSize: Theme.fontXL
        font.weight: Font.Bold
    }

    // ---- category rail ----
    Flickable {
        id: navRail
        anchors.top: header.bottom
        anchors.topMargin: Theme.spacingL
        anchors.left: parent.left
        anchors.leftMargin: Theme.spacingXL
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spacingL
        width: 170
        clip: true
        contentWidth: width
        contentHeight: navCol.height
        boundsBehavior: Flickable.StopAtBounds
        Column {
            id: navCol
            width: parent.width
            spacing: 2
            Repeater {
                model: page.tabNames
                delegate: NavItem {
                    required property int index
                    required property string modelData
                    label: modelData
                    iconName: page.tabIcons[index]
                    active: page.tab === index
                    onClicked: page.tab = index
                }
            }
        }
    }

    // ---- panes ----
    Item {
        id: paneArea
        anchors.top: header.bottom
        anchors.topMargin: Theme.spacingL
        anchors.left: navRail.right
        anchors.leftMargin: Theme.spacingXL
        anchors.right: parent.right
        anchors.rightMargin: Theme.spacingL
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spacingL

        // Only the visible pane is instantiated; leaving it unloads it (Main.qml
        // page idiom — no search here, so no need to keep panes latched).

        // --- Recording ---
        Loader {
            anchors.fill: parent
            active: page.tab === 0
            visible: active
            sourceComponent: ScrollPane {
                // Probe click-capture permission once when this pane first opens so
                // the status row is accurate without a startup cost.
                Component.onCompleted: if (Studio.inputPermissionStatus < 0) Studio.refreshInputPermission()

                UCard {
                    width: page.cardWidth
                    Column {
                        width: parent.width
                        spacing: Theme.spacingS
                        SectionTitle { text: qsTr("Recording") }
                        SettingRow {
                            label: qsTr("Frame rate")
                            help: qsTr("Frames captured per second.")
                            UComboBox {
                                width: 110
                                model: ["30", "60"]
                                readonly property var ids: [30, 60]
                                currentIndex: Math.max(0, ids.indexOf(Studio.settings.recordFps))
                                onActivated: (i) => Studio.settings.recordFps = ids[i]
                            }
                        }
                        SettingRow {
                            label: qsTr("Quality")
                            help: qsTr("Higher quality means bigger files (H.264 CRF %1).").arg(Studio.settings.masterCrf)
                            USlider {
                                width: 220
                                from: page.crfMin; to: page.crfMax
                                // Inverted: slider right = higher quality = lower CRF.
                                value: page.crfMin + page.crfMax - Studio.settings.masterCrf
                                onMoved: (v) => Studio.settings.masterCrf = page.crfMin + page.crfMax - Math.round(v)
                            }
                        }
                        SettingRow {
                            label: qsTr("System audio")
                            help: qsTr("Record desktop sound.")
                            USwitch {
                                checked: Studio.settings.recordSystemAudio
                                onToggled: (c) => Studio.settings.recordSystemAudio = c
                            }
                        }
                        SettingRow {
                            label: qsTr("Microphone")
                            help: qsTr("Record from your microphone.")
                            USwitch {
                                checked: Studio.settings.recordMicrophone
                                onToggled: (c) => Studio.settings.recordMicrophone = c
                            }
                        }
                        SettingRow {
                            label: qsTr("Countdown")
                            help: qsTr("Seconds before capture begins.")
                            UValueCombo {
                                width: 120
                                values: [0, 3, 5, 10]
                                from: 0; to: 60
                                suffix: qsTr(" s")
                                value: Studio.settings.recordCountdownSec
                                onChanged: (v) => Studio.settings.recordCountdownSec = v
                            }
                        }
                        SettingRow {
                            label: qsTr("Hide window while recording")
                            help: qsTr("The main window disappears during capture and returns when you stop.")
                            USwitch {
                                checked: Studio.settings.hideWindowWhileRecording
                                onToggled: (c) => Studio.settings.hideWindowWhileRecording = c
                            }
                        }
                        SettingRow {
                            label: qsTr("Collapse recording HUD")
                            help: qsTr("Shrink the HUD to a thin bottom-edge sliver while recording so it barely shows in the capture. Hover it (or pause) to expand.")
                            USwitch {
                                checked: Studio.settings.hudCollapseWhileRecording
                                onToggled: (c) => Studio.settings.hudCollapseWhileRecording = c
                            }
                        }
                    }
                }

                UCard {
                    width: page.cardWidth
                    Column {
                        id: wcCol
                        width: parent.width
                        spacing: Theme.spacingS
                        // Re-checked when the pane opens and when the path is edited.
                        property bool webcamPresent: false
                        Component.onCompleted: webcamPresent = Studio.webcamDeviceAvailable(Studio.settings.webcamDevice)
                        SectionTitle { text: qsTr("Webcam") }
                        SettingRow {
                            label: qsTr("Record webcam")
                            help: wcCol.webcamPresent
                                  ? qsTr("Capture %1 into a sidecar, shown as an overlay in the editor.").arg(Studio.settings.webcamDevice)
                                  : qsTr("No camera found at %1.").arg(Studio.settings.webcamDevice)
                            USwitch {
                                enabled: wcCol.webcamPresent
                                checked: Studio.settings.recordWebcam && wcCol.webcamPresent
                                onToggled: (c) => Studio.settings.recordWebcam = c
                            }
                        }
                        SettingRow {
                            label: qsTr("Camera device")
                            UTextField {
                                width: 180
                                text: Studio.settings.webcamDevice
                                onEdited: (t) => {
                                    Studio.settings.webcamDevice = t
                                    wcCol.webcamPresent = Studio.webcamDeviceAvailable(t)
                                }
                            }
                        }
                    }
                }

                UCard {
                    width: page.cardWidth
                    Column {
                        width: parent.width
                        spacing: Theme.spacingS
                        SectionTitle { text: qsTr("Mouse clicks") }
                        SettingRow {
                            label: qsTr("Capture mouse clicks")
                            help: qsTr("Record click timings to drive automatic zoom.")
                            USwitch {
                                checked: Studio.settings.clickCaptureEnabled
                                onToggled: (c) => Studio.settings.clickCaptureEnabled = c
                            }
                        }

                        // Input-permission status + fix hint.
                        Item {
                            width: parent.width
                            height: 44
                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: page.permText()
                                color: page.permColor()
                                font.pixelSize: Theme.fontM
                                font.weight: Font.DemiBold
                            }
                            UIconButton {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                iconName: "view-refresh"
                                tooltip: qsTr("Re-check")
                                onClicked: Studio.refreshInputPermission()
                            }
                        }

                        // No-permission: show the copyable fix command + a note.
                        Column {
                            width: parent.width
                            spacing: Theme.spacingS
                            visible: Studio.inputPermissionStatus === page.permNoPermission
                            Row {
                                width: parent.width
                                spacing: Theme.spacingS
                                UTextField {
                                    id: hintField
                                    width: parent.width - copyBtn.width - Theme.spacingS
                                    readOnly: true
                                    text: Studio.inputPermissionFixHint()
                                }
                                UButton {
                                    id: copyBtn
                                    text: qsTr("Copy")
                                    variant: "tonal"
                                    onClicked: Studio.copyToClipboard(hintField.text)
                                }
                            }
                            Text {
                                width: parent.width
                                wrapMode: Text.WordWrap
                                text: qsTr("Re-login required after joining the group. Input-group access lets software observe input devices; Studio only records mouse button timings while you record.")
                                color: Theme.textTertiary
                                font.pixelSize: Theme.fontS
                            }
                        }

                        // Not built.
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            visible: Studio.inputPermissionStatus === page.permNotBuilt
                            text: qsTr("This build has no input-device support, so mouse clicks won't be recorded.")
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontS
                        }
                    }
                }
            }
        }

        // --- Storage ---
        Loader {
            anchors.fill: parent
            active: page.tab === 1
            visible: active
            sourceComponent: ScrollPane {
                UCard {
                    width: page.cardWidth
                    Column {
                        width: parent.width
                        spacing: Theme.spacingS
                        SectionTitle { text: qsTr("Storage") }
                        Text {
                            text: qsTr("Projects folder")
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontM
                        }
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: qsTr("Where recordings and their projects are saved.")
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontS
                        }
                        Item { width: 1; height: Theme.spacingXS }
                        Row {
                            width: parent.width
                            spacing: Theme.spacingS
                            UTextField {
                                width: parent.width - browseBtn.width - Theme.spacingS
                                readOnly: true
                                text: Studio.settings.projectsDirectory
                            }
                            UButton {
                                id: browseBtn
                                text: qsTr("Browse…")
                                variant: "tonal"
                                onClicked: {
                                    var d = Studio.pickProjectsDirectory(Studio.settings.projectsDirectory)
                                    if (d !== "")
                                        Studio.settings.projectsDirectory = d
                                }
                            }
                        }
                    }
                }
            }
        }

        // --- Appearance ---
        Loader {
            anchors.fill: parent
            active: page.tab === 2
            visible: active
            sourceComponent: ScrollPane {
                UCard {
                    width: page.cardWidth
                    Column {
                        width: parent.width
                        spacing: Theme.spacingM
                        SectionTitle { text: qsTr("Appearance") }
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: qsTr("Color theme for the whole app. “System” follows your desktop's light/dark scheme.")
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontS
                        }
                        Grid {
                            columns: 3
                            spacing: Theme.spacingM
                            Repeater {
                                model: page.themeIds
                                delegate: Rectangle {
                                    id: themeCard
                                    required property int index
                                    required property string modelData
                                    readonly property bool current: ThemeController.themeName === modelData
                                    width: 132; height: 88
                                    radius: Theme.radiusM
                                    color: current ? Theme.alpha(Theme.accent, 0.16) : Theme.surface
                                    border.width: current ? 2 : 1
                                    border.color: current ? Theme.accent : Theme.divider
                                    Column {
                                        anchors.centerIn: parent
                                        spacing: 10
                                        Row {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            spacing: 6
                                            Repeater {
                                                model: page.seedsFor(themeCard.modelData)
                                                delegate: ColorDot { dotColor: modelData }
                                            }
                                        }
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: page.themeNames[themeCard.index]
                                            color: Theme.textPrimary
                                            font.pixelSize: Theme.fontS
                                            font.weight: themeCard.current ? Font.DemiBold : Font.Normal
                                        }
                                    }
                                    // Whole-card select (sits above the decorative dots).
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: ThemeController.themeName = themeCard.modelData
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // --- About ---
        Loader {
            anchors.fill: parent
            active: page.tab === 3
            visible: active
            sourceComponent: ScrollPane {
                UCard {
                    width: page.cardWidth
                    Column {
                        width: parent.width
                        spacing: Theme.spacingM
                        Row {
                            spacing: Theme.spacingL
                            Image {
                                source: "qrc:/resources/icons/unisic-studio.svg"
                                sourceSize: Qt.size(64, 64)
                                width: 64; height: 64
                                smooth: true
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Column {
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 4
                                Text {
                                    text: qsTr("Unisic Studio")
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontXL
                                    font.weight: Font.Bold
                                }
                                Text {
                                    text: qsTr("Version %1%2").arg(Studio.version)
                                          .arg(Studio.devBuild ? qsTr(" · dev") : "")
                                    color: Theme.textSecondary
                                    font.pixelSize: Theme.fontM
                                }
                            }
                        }
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: qsTr("Free software under the GNU General Public License v3. Zero telemetry.")
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontS
                        }
                        Row {
                            spacing: Theme.spacingS
                            UIcon { name: "internet-services"; size: 16; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
                            Text {
                                id: link
                                anchors.verticalCenter: parent.verticalCenter
                                text: "github.com/unisic"
                                color: Theme.accent
                                font.pixelSize: Theme.fontM
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Qt.openUrlExternally("https://github.com/unisic")
                                }
                            }
                        }
                    }
                }
            }
        }

        // --- Developer (dev builds only, tab 4) ---
        Loader {
            anchors.fill: parent
            active: Studio.devBuild && page.tab === 4
            visible: active
            sourceComponent: ScrollPane {
                // Per-action buttons: every user-facing path gets a dev trigger.
                UCard {
                    width: page.cardWidth
                    Column {
                        width: parent.width
                        spacing: Theme.spacingM
                        SectionTitle { text: qsTr("Developer") }
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: qsTr("Exercise each path in isolation. Dev builds only.")
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontS
                        }
                        Flow {
                            width: parent.width
                            spacing: Theme.spacingS
                            UButton { text: qsTr("Import test video"); variant: "tonal"; compact: true
                                      onClicked: Studio.devImportTestVideo() }
                            UButton { text: qsTr("Open recording HUD"); variant: "tonal"; compact: true
                                      onClicked: Studio.devOpenRecordingHud() }
                            UButton { text: qsTr("Run export test"); variant: "tonal"; compact: true
                                      onClicked: Studio.devRunExportTest() }
                            UButton { text: qsTr("Run auto-zoom test"); variant: "tonal"; compact: true
                                      onClicked: Studio.devRunAutozoomTest() }
                        }
                    }
                }

                // Smoke test: run + live transcript (mirrors Unisic's dev pane).
                UCard {
                    width: page.cardWidth
                    Column {
                        width: parent.width
                        spacing: Theme.spacingM
                        SectionTitle { text: qsTr("Smoke test") }
                        Row {
                            width: parent.width
                            spacing: Theme.spacingS
                            UButton {
                                text: qsTr("Run full smoke test (F8)")
                                variant: "filled"
                                compact: true
                                enabled: !Studio.smokeTestRunning
                                onClicked: Studio.runSmokeTest()
                            }
                            UButton {
                                text: qsTr("Copy log")
                                variant: "ghost"
                                compact: true
                                visible: Studio.smokeTestLog !== ""
                                onClicked: Studio.copyToClipboard(Studio.smokeTestLog)
                            }
                        }
                        Rectangle {
                            width: parent.width
                            height: 220
                            visible: Studio.smokeTestLog !== ""
                            radius: Theme.radiusM
                            color: Theme.backgroundDeep
                            border.width: 1
                            border.color: Theme.divider
                            Flickable {
                                id: logFlick
                                anchors.fill: parent
                                anchors.margins: Theme.spacingM
                                contentHeight: devLog.implicitHeight
                                clip: true
                                onContentHeightChanged: contentY = Math.max(0, contentHeight - height)
                                Text {
                                    id: devLog
                                    width: logFlick.width - 2 * Theme.spacingM
                                    text: Studio.smokeTestLog
                                    color: Theme.textSecondary
                                    font.family: "monospace"
                                    font.pixelSize: Theme.fontS
                                    wrapMode: Text.WrapAnywhere
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
