import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Effects
import Unisic.Kit
import UnisicStudio

// Main window: themed chrome, a sidebar (Projects / Settings) and either the
// recent-projects area or the Settings page. "New Recording" drives the capture
// flow (the recording HUD is a separate C++-created window; the main window can
// hide itself while a recording is live). Every user-visible string is wrapped in
// qsTr(); every color comes from a Theme token; every control is a kit component.
Window {
    id: window
    // Restore the last windowed size (the settings exist for exactly this; a
    // user resize overwrites the binding, which is the intended one-shot use).
    width: Studio.settings.windowWidth
    height: Studio.settings.windowHeight
    minimumWidth: 1000
    minimumHeight: 640
    visible: true
    title: qsTr("Unisic Studio")
    color: Theme.backgroundDeep

    // Persist plain windowed resizes only — never a maximized/fullscreen size.
    onWidthChanged: if (visibility === Window.Windowed) Studio.settings.windowWidth = width
    onHeightChanged: if (visibility === Window.Windowed) Studio.settings.windowHeight = height

    property int currentPage: 0

    function mmss(ms) {
        if (isNaN(ms) || ms < 0) ms = 0
        var s = Math.floor(ms / 1000)
        var m = Math.floor(s / 60)
        s = s % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    // Hide the main window while a recording is live so the shell doesn't land in
    // the capture; it returns on stop/cancel/fail. The recording HUD is a separate
    // always-on-top window (C++ HudManager), so it stays visible when this hides.
    Connections {
        target: Studio
        function onRecorderStateChanged() {
            if (!Studio.settings.hideWindowWhileRecording)
                return
            if (Studio.recorderState === StudioApp.RecIdle)
                window.show()   // show (don't raise): a just-opened editor stays in front
            else
                window.hide()
        }
    }

    Rectangle { // content backdrop with a subtle vertical falloff
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.background }
            GradientStop { position: 1.0; color: Theme.backgroundDeep }
        }
    }

    Rectangle { // sidebar
        id: sidebar
        width: 224
        height: parent.height
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.lighter(Theme.primary, 1.12) }
            GradientStop { position: 1.0; color: Theme.primary }
        }
        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Theme.shadow
            shadowBlur: 1.0
            shadowHorizontalOffset: 3
            shadowOpacity: 0.5
        }
        z: 2

        Column {
            anchors.top: parent.top
            anchors.topMargin: Theme.spacingXL
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.spacingM
            anchors.rightMargin: Theme.spacingM
            spacing: 4

            Row { // app logo + name
                spacing: 10
                anchors.horizontalCenter: parent.horizontalCenter
                Image {
                    source: "qrc:/resources/icons/unisic-studio.svg"
                    sourceSize: Qt.size(34, 34)
                    width: 34; height: 34
                    smooth: true
                    anchors.verticalCenter: parent.verticalCenter
                    // Dev builds render the mark gray everywhere, at a glance.
                    layer.enabled: Studio.devBuild
                    layer.effect: MultiEffect { saturation: -1.0 }
                }
                Text {
                    text: qsTr("Unisic Studio")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontL
                    font.weight: Font.Bold
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Item { width: 1; height: Theme.spacingXL }

            SidebarItem {
                iconName: "folder-open"
                label: qsTr("Projects")
                active: window.currentPage === 0
                onClicked: window.currentPage = 0
            }
            SidebarItem {
                iconName: "configure"
                label: qsTr("Settings")
                active: window.currentPage === 1
                onClicked: window.currentPage = 1
            }
        }

        // Version / build footer.
        Text {
            id: versionLabel
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Theme.spacingM
            anchors.horizontalCenter: parent.horizontalCenter
            horizontalAlignment: Text.AlignHCenter
            text: "v" + Studio.version + (Studio.devBuild ? " · dev" : "")
            color: Theme.textTertiary
            font.pixelSize: Theme.fontS
        }
    }

    // Settings page (lazy): shown when the Settings sidebar entry is active.
    Loader {
        anchors.left: sidebar.right
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        active: window.currentPage === 1
        visible: active
        sourceComponent: SettingsPage {}
    }

    // Main area: recent-projects grid, or a centered empty state when none.
    Item {
        id: content
        anchors.left: sidebar.right
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: window.currentPage === 0

        readonly property bool hasRecents: Studio.recentProjects.length > 0

        // ---- Empty state ----
        Column {
            visible: !content.hasRecents
            anchors.verticalCenter: parent.verticalCenter
            x: (parent.width - width) / 2
            width: Math.min(440, parent.width - 2 * Theme.spacingXL)
            spacing: Theme.spacingL

            UCard {
                width: Math.min(340, parent.width)
                height: 176
                anchors.horizontalCenter: parent.horizontalCenter
                padding: Theme.spacingM
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusM
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Theme.primary }
                        GradientStop { position: 1.0; color: Theme.secondary }
                    }
                    Rectangle {
                        width: parent.width * 0.70
                        height: parent.height * 0.68
                        anchors.centerIn: parent
                        radius: Theme.radiusM
                        color: Theme.backgroundDeep
                        border.width: 1
                        border.color: Theme.edgeLight
                        Rectangle {
                            width: parent.width * 0.34
                            height: parent.height * 0.42
                            x: parent.width * 0.48
                            y: parent.height * 0.22
                            radius: Theme.radiusS
                            color: Theme.alpha(Theme.accent, 0.12)
                            border.width: 2
                            border.color: Theme.accent
                        }
                        UIcon {
                            name: "arrow-up"
                            rotation: -35
                            size: 25
                            color: Theme.textPrimary
                            x: parent.width * 0.63
                            y: parent.height * 0.42
                        }
                    }
                }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Polished motion, automatically")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontXL
                font.weight: Font.Bold
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                horizontalAlignment: Text.AlignHCenter
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Record or import a screen capture. Studio frames clicks, glides the cursor, and builds a presentation-ready edit.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontM
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.spacingM
                UButton {
                    text: qsTr("New Recording")
                    variant: "filled"
                    iconName: "media-record"
                    onClicked: Studio.startRecording()
                }
                UButton {
                    text: qsTr("Import Video…")
                    variant: "tonal"
                    iconName: "image"
                    onClicked: Studio.importVideo()
                }
            }
        }

        // ---- Recent-projects view ----
        Item {
            visible: content.hasRecents
            anchors.fill: parent
            anchors.margins: Theme.spacingXL

            Item {
                id: recentsHeader
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 40
                Text {
                    text: qsTr("Projects")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontXL
                    font.weight: Font.Bold
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                }
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    spacing: Theme.spacingM
                    UButton {
                        text: qsTr("New Recording")
                        variant: "ghost"
                        iconName: "media-record"
                        onClicked: Studio.startRecording()
                    }
                    UButton {
                        text: qsTr("Import Video…")
                        variant: "filled"
                        iconName: "image"
                        onClicked: Studio.importVideo()
                    }
                }
            }

            GridView {
                id: grid
                anchors.top: recentsHeader.bottom
                anchors.topMargin: Theme.spacingL
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                clip: true
                cellWidth: 240
                cellHeight: 132
                model: Studio.recentProjects

                delegate: Item {
                    width: grid.cellWidth
                    height: grid.cellHeight
                    UCard {
                        anchors.fill: parent
                        anchors.margins: Theme.spacingS
                        Column {
                            width: parent.width
                            spacing: Theme.spacingS
                            Text {
                                width: parent.width
                                text: modelData.name
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontM
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Text {
                                text: window.mmss(modelData.durationMs)
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontS
                            }
                            Text {
                                text: qsTr("Opened %1").arg(
                                          new Date(modelData.lastOpened).toLocaleDateString(Qt.locale()))
                                color: Theme.textTertiary
                                font.pixelSize: Theme.fontS
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Studio.openProject(modelData.path)
                        }
                    }
                }
            }
        }
    }

    // Dev-only smoke test: F8 opens the results dialog and runs the sequence.
    Shortcut {
        sequence: "F8"
        enabled: Studio.devBuild
        onActivated: smokeDialog.run()
    }
    SmokeTestDialog { id: smokeDialog }

    // Transient toast for Studio.notified (import/save feedback).
    Rectangle {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spacingXL
        radius: Theme.radiusM
        color: toast.error ? Theme.danger : Theme.surfaceTop
        opacity: 0
        visible: opacity > 0
        width: msg.implicitWidth + 2 * Theme.spacingL
        height: msg.implicitHeight + 2 * Theme.spacingM
        z: 100

        property bool error: false

        Text {
            id: msg
            anchors.centerIn: parent
            color: toast.error ? Theme.dangerText : Theme.textPrimary
            font.pixelSize: Theme.fontM
        }
        Behavior on opacity { NumberAnimation { duration: Theme.animMed } }
        Timer { id: toastTimer; interval: 3200; onTriggered: toast.opacity = 0 }

        Connections {
            target: Studio
            function onNotified(message, isError) {
                msg.text = message
                toast.error = isError
                toast.opacity = 1
                toastTimer.restart()
            }
        }
    }
}
