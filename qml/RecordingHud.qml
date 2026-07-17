import QtQuick
import QtQuick.Window
import QtQuick.Effects
import Unisic.Kit
import UnisicStudio

// The always-on-top recording HUD: a separate frameless pill window created by
// C++ HudManager (its own QQmlContext, NOT parented into StudioMain) while a
// recording is live. It reads the recorder state straight off the `Studio` facade
// context property and drives stop / pause / cancel back through it.
//
// Positioning caveat: a plain Wayland xdg-toplevel cannot position itself — the
// x/y below is only honoured on X11 / XWayland (KWin-X11). Under native Wayland
// the compositor places the window (usually centered); we accept that in v1. A
// proper bottom-center anchor needs wlr-layer-shell, a future unisic-kit addition.
Window {
    id: hud

    readonly property int pillW: 360
    readonly property int pillH: 56
    // The cancel confirm is a centered Popup; grow the window while it's open so it
    // isn't clipped by the 56px pill, keeping the pill pinned to the bottom edge.
    readonly property bool confirming: cancelDialog.visible

    width: pillW
    height: confirming ? 200 : pillH
    visible: true
    color: "transparent"
    title: qsTr("Recording")

    // Frameless, on top, off the taskbar (Tool), and NON-focusable so it never
    // steals keyboard focus from the app being recorded.
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
           | Qt.Tool | Qt.WindowDoesNotAcceptFocus

    // Bottom-center of the screen the compositor puts us on (see caveat above).
    x: Screen.virtualX + Math.round((Screen.width - width) / 2)
    y: Screen.virtualY + Screen.height - height - 48

    // --- recorder state, read from the facade (StudioApp enum) ---
    readonly property int st: Studio.recorderState
    readonly property bool isCountdown: st === StudioApp.RecCountdown
    readonly property bool isPaused: st === StudioApp.RecPaused
    readonly property bool isRecording: st === StudioApp.RecRecording
    readonly property bool isFinalizing: st === StudioApp.RecFinalizing
    // Green only when the user asked for click capture AND libinput can observe
    // devices (inputPermissionStatus 0 == InputPermission::Available).
    readonly property bool clickActive: Studio.settings.clickCaptureEnabled
                                        && Studio.inputPermissionStatus === 0

    function mmss(total) {
        if (isNaN(total) || total < 0) total = 0
        var m = Math.floor(total / 60)
        var s = total % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    // The pill.
    Rectangle {
        id: pill
        width: hud.pillW
        height: hud.pillH
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        radius: height / 2
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.surfaceTop }
            GradientStop { position: 1.0; color: Theme.surface }
        }
        border.width: 1
        border.color: Theme.divider

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Theme.shadow
            shadowBlur: 1.0
            shadowVerticalOffset: 3
            shadowOpacity: 0.5
        }

        // ---- Countdown phase: just the big number + cancel ----
        Item {
            anchors.fill: parent
            visible: hud.isCountdown
            Text {
                anchors.centerIn: parent
                text: Studio.recorderCountdown > 0 ? Studio.recorderCountdown : qsTr("Go")
                color: Theme.textPrimary
                font.pixelSize: 30
                font.weight: Font.Bold
            }
            UIconButton {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                iconName: "window-close"
                tooltip: qsTr("Cancel")
                onClicked: cancelDialog.open()
            }
        }

        // ---- Recording / paused / finalizing phase ----
        Item {
            anchors.fill: parent
            visible: !hud.isCountdown

            Row { // left: REC dot + elapsed (or "Saving…")
                anchors.left: parent.left
                anchors.leftMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                Rectangle {
                    id: recDot
                    width: 12; height: 12; radius: 6
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.danger
                    // Blink while actively recording; solid when paused/finalizing.
                    property bool blink: hud.visible && hud.isRecording
                    property real blinkOpacity: 1.0
                    opacity: blink ? blinkOpacity : 1.0
                    SequentialAnimation {
                        // Bound to `blink`, so it stops the moment the HUD hides,
                        // pauses, or is torn down — no timer survives teardown.
                        running: recDot.blink
                        loops: Animation.Infinite
                        NumberAnimation { target: recDot; property: "blinkOpacity"; to: 0.25; duration: 550; easing.type: Easing.InOutSine }
                        NumberAnimation { target: recDot; property: "blinkOpacity"; to: 1.0; duration: 550; easing.type: Easing.InOutSine }
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: hud.isFinalizing ? qsTr("Saving…") : hud.mmss(Studio.recorderElapsed)
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontL
                    font.weight: Font.DemiBold
                }
            }

            Row { // right: click-capture status + transport
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6
                visible: !hud.isFinalizing

                Item { // click-capture status dot (centered, with a hover tip)
                    width: 16; height: 38
                    Rectangle {
                        id: clickDot
                        width: 10; height: 10; radius: 5
                        anchors.centerIn: parent
                        color: hud.clickActive ? Theme.success : Theme.textTertiary
                        MouseArea {
                            id: clickHov
                            anchors.fill: parent
                            hoverEnabled: true
                        }
                    }
                    UHoverTip {
                        anchor: clickDot
                        show: clickHov.containsMouse
                        text: hud.clickActive ? qsTr("Capturing mouse clicks")
                                              : qsTr("Mouse-click capture is off or unavailable")
                    }
                }

                UIconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: hud.isPaused ? "media-playback-start" : "media-playback-pause"
                    tooltip: hud.isPaused ? qsTr("Resume") : qsTr("Pause")
                    onClicked: Studio.togglePauseRecording()
                }
                UIconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "media-playback-stop"
                    active: true
                    tooltip: qsTr("Stop")
                    onClicked: Studio.stopRecording()
                }
                UIconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "window-close"
                    tooltip: qsTr("Cancel")
                    onClicked: cancelDialog.open()
                }
            }
        }
    }

    UConfirmDialog {
        id: cancelDialog
        title: qsTr("Discard recording?")
        text: qsTr("The current recording will be deleted and cannot be recovered.")
        confirmText: qsTr("Discard")
        cancelText: qsTr("Keep recording")
        destructive: true
        onAccepted: Studio.cancelRecording()
    }
}
