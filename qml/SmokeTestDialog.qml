import QtQuick
import QtQuick.Controls
import Unisic.Kit

// Dev-only smoke-test results dialog: a modal popup with a live monospace
// transcript of the sequential PASS/FAIL/SKIP lines from Studio.runSmokeTest().
// Bound directly to Studio.smokeTestLog (updated line-by-line), so it fills in
// as the run progresses. open() kicks a run off if one isn't already going.
Popup {
    id: root

    function run() {
        open()
        if (!Studio.smokeTestRunning)
            Studio.runSmokeTest()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(560, parent ? parent.width - 2 * Theme.spacingXL : 560)
    padding: Theme.spacingXL

    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.45) }
    background: Rectangle {
        radius: Theme.radiusL
        color: Theme.surface
        border.width: 1
        border.color: Theme.divider
    }

    contentItem: Column {
        spacing: Theme.spacingL

        Row {
            width: parent.width
            Text {
                text: qsTr("Smoke test")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontL
                font.weight: Font.DemiBold
                width: parent.width - spinner.width
                anchors.verticalCenter: parent.verticalCenter
            }
            BusyIndicator {
                id: spinner
                running: Studio.smokeTestRunning
                visible: running
                width: 22; height: 22
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        // Live transcript.
        Rectangle {
            width: parent.width
            height: 280
            radius: Theme.radiusM
            color: Theme.backgroundDeep
            border.width: 1
            border.color: Theme.divider
            Flickable {
                id: flick
                anchors.fill: parent
                anchors.margins: Theme.spacingM
                contentHeight: logText.implicitHeight
                clip: true
                // Auto-scroll to the newest line as the log grows.
                onContentHeightChanged: contentY = Math.max(0, contentHeight - height)
                Text {
                    id: logText
                    width: flick.width - 2 * Theme.spacingM
                    text: Studio.smokeTestLog
                    color: Theme.textSecondary
                    font.family: "monospace"
                    font.pixelSize: Theme.fontS
                    wrapMode: Text.WrapAnywhere
                }
            }
        }

        Row {
            anchors.right: parent.right
            spacing: Theme.spacingS
            UButton {
                text: qsTr("Run again")
                variant: "tonal"
                compact: true
                enabled: !Studio.smokeTestRunning
                onClicked: Studio.runSmokeTest()
            }
            UButton {
                text: qsTr("Copy")
                variant: "ghost"
                compact: true
                enabled: Studio.smokeTestLog !== ""
                onClicked: Studio.copyToClipboard(Studio.smokeTestLog)
            }
            UButton {
                text: qsTr("Close")
                variant: "filled"
                compact: true
                onClicked: root.close()
            }
        }
    }
}
