import QtQuick
import QtQuick.Window
import Unisic.Kit

// Shared custom title bar for the app's frameless windows (StudioMain and the
// per-recording EditorWindow) so their chrome is pixel-identical: a Theme
// gradient strip with the window/app title on the left and minimize / maximize /
// close buttons (kit UIconButton) on the right. The whole strip is a drag handle
// — past a small threshold it hands the move to the compositor via
// startSystemMove() — and a double-click toggles maximize. Set `window` to the
// hosting Window; `title` defaults to that window's own title. Every user-facing
// string is qsTr()'d; every colour is a Theme token.
Rectangle {
    id: root

    // The frameless Window this bar decorates. Drag, the three buttons and the
    // double-click-maximize all route to it.
    required property var window
    // Title text shown at the left; defaults to the hosting window's title.
    property string title: root.window ? root.window.title : ""

    height: 38
    z: 20
    gradient: Gradient {
        GradientStop { position: 0.0; color: Qt.lighter(Theme.primary, 1.12) }
        GradientStop { position: 1.0; color: Theme.primary }
    }

    // Maximize/restore toggle — shared by the double-click and the button.
    function toggleMaximize() {
        if (!root.window)
            return
        if (root.window.visibility === Window.Maximized)
            root.window.showNormal()
        else
            root.window.showMaximized()
    }

    // Drag anywhere on the bar to move the window (a small threshold keeps a plain
    // click from registering as a stray move); double-click toggles maximize.
    MouseArea {
        anchors.fill: parent
        property real pressX: 0
        property real pressY: 0
        property bool moving: false
        onPressed: (m) => { pressX = m.x; pressY = m.y; moving = false }
        onPositionChanged: (m) => {
            if (!moving && (Math.abs(m.x - pressX) > 6 || Math.abs(m.y - pressY) > 6)) {
                moving = true
                if (root.window) root.window.startSystemMove()
            }
        }
        onDoubleClicked: root.toggleMaximize()
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: Theme.spacingL
        anchors.verticalCenter: parent.verticalCenter
        text: root.title
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
            tooltip: qsTr("Minimize"); onClicked: if (root.window) root.window.showMinimized()
        }
        UIconButton {
            iconName: "window"; iconSize: 13; width: 30; height: 30
            tooltip: qsTr("Maximize")
            onClicked: root.toggleMaximize()
        }
        UIconButton {
            iconName: "close"; iconSize: 14; width: 30; height: 30
            tooltip: qsTr("Close"); onClicked: if (root.window) root.window.close()
        }
    }
}
