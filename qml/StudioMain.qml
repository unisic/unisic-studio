import QtQuick
import QtQuick.Window
import QtQuick.Effects
import Unisic.Kit

// M0 skeleton main window: themed chrome, a sidebar with two entries, and a
// centered empty state. No recording/editing/export yet — the two primary
// actions are deliberately disabled with "coming in" tooltips. Every
// user-visible string is wrapped in qsTr(); every color comes from a Theme
// token (zero hardcoded hex); every control is a kit component.
Window {
    id: window
    width: 1280
    height: 800
    minimumWidth: 1000
    minimumHeight: 640
    visible: true
    title: qsTr("Unisic Studio")
    color: Theme.backgroundDeep

    property int currentPage: 0

    // A kit UButton disabled via `enabled: false` also disables its own
    // MouseArea, so it can't drive a hover tooltip. Wrap it: the button shows
    // disabled, an overlaid hover MouseArea drives a UHoverTip explaining when
    // the action arrives.
    component DisabledAction: Item {
        id: da
        property alias text: btn.text
        property string variant: "filled"
        property string iconName: ""
        property string tip: ""
        implicitWidth: btn.implicitWidth
        implicitHeight: btn.implicitHeight
        UButton {
            id: btn
            anchors.centerIn: parent
            variant: da.variant
            iconName: da.iconName
            enabled: false
        }
        MouseArea {
            id: hov
            anchors.fill: parent
            hoverEnabled: true
        }
        UHoverTip { anchor: da; text: da.tip; show: hov.containsMouse }
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
            // Non-functional placeholder for M0 — the Settings page lands later.
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

    // Main area: centered empty state.
    Column {
        anchors.verticalCenter: parent.verticalCenter
        // Center within the area to the right of the sidebar.
        x: sidebar.width + (parent.width - sidebar.width - width) / 2
        width: Math.min(440, parent.width - sidebar.width - 2 * Theme.spacingXL)
        spacing: Theme.spacingL

        UIcon {
            name: "monitor"
            color: Theme.textTertiary
            size: 72
            anchors.horizontalCenter: parent.horizontalCenter
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("No projects yet")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontXL
            font.weight: Font.Bold
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            horizontalAlignment: Text.AlignHCenter
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Record your screen or import a video to get started")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontM
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: Theme.spacingM

            DisabledAction {
                text: qsTr("New Recording")
                variant: "filled"
                iconName: "media-record"
                tip: qsTr("Coming in M2")
            }
            DisabledAction {
                text: qsTr("Import Video…")
                variant: "tonal"
                iconName: "image"
                tip: qsTr("Coming in M1")
            }
        }
    }
}
