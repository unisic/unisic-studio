import QtQuick
import QtQuick.Window
import QtQuick.Effects
import Unisic.Kit
import UnisicStudio

// The recording HUD: a separate frameless window created by C++ HudManager (its
// own QQmlContext, NOT parented into StudioMain) while a recording is live. It
// reads recorder state off the `Studio` facade and drives stop / pause / cancel
// back through it.
//
// Placement: HudManager makes this a wlr-layer-shell OVERLAY surface anchored
// bottom-centre of the recorded screen (KWin/wlroots) so it sits above every
// window. It must therefore stay `visible: false` here — C++ sets the layer-shell
// role BEFORE the first show(). Where layer-shell is absent (GNOME, or a build
// without it) it is a plain xdg-toplevel: it can't self-position or stay on top,
// which we accept (documented in the README).
//
// Burn-in mitigation: a Wayland screencast CANNOT exclude this surface from the
// capture, so after commit the HUD collapses to a thin bottom-edge sliver that is
// barely visible in the recorded file. Hovering it — or pausing — expands it back
// to the full pill. Gated by Settings → Recording → "Collapse recording HUD".
// (The countdown pill is NOT recorded: encoding starts at commit, AFTER the
// countdown, so it is shown large and clear.)
Window {
    id: hud

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

    property bool hovered: hoverHandler.hovered
    readonly property bool confirming: cancelDialog.visible
    // Collapse to the sliver only while actively recording (never paused/countdown/
    // finalizing), when the setting is on, and not hovered or confirming.
    readonly property bool collapsed: isRecording && Studio.settings.hudCollapseWhileRecording
                                      && !hovered && !confirming

    // Full-size footprint: an enlarged pill during the (unrecorded) countdown.
    readonly property int fullW: isCountdown ? 240 : 360
    readonly property int fullH: isCountdown ? 104 : 56

    width: collapsed ? 64 : fullW
    height: confirming ? 210 : (collapsed ? 18 : fullH)
    Behavior on width  { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
    Behavior on height { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

    visible: false     // HudManager shows us AFTER configuring the layer-shell role
    color: "transparent"
    title: qsTr("Recording")

    // Frameless, on top, off the taskbar (Tool), and NON-focusable so it never
    // steals keyboard focus from the app being recorded.
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
           | Qt.Tool | Qt.WindowDoesNotAcceptFocus

    // Honoured only on the plain-toplevel fallback (X11/XWayland). Under layer-shell
    // the compositor anchors us per hudPlacement and ignores x/y.
    readonly property string _place: Studio.settings.hudPlacement
    x: {
        var edge = 48
        if (_place.endsWith("Left"))  return Screen.virtualX + edge
        if (_place.endsWith("Right")) return Screen.virtualX + Screen.width - width - edge
        return Screen.virtualX + Math.round((Screen.width - width) / 2)
    }
    y: _place.startsWith("top") ? Screen.virtualY + 48
                                : Screen.virtualY + Screen.height - height - 48

    function mmss(total) {
        if (isNaN(total) || total < 0) total = 0
        var m = Math.floor(total / 60)
        var s = total % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    Item {
        anchors.fill: parent
        HoverHandler { id: hoverHandler }

        // ---- Collapsed sliver: a minimal bottom-edge handle (barely visible in
        //      the capture). Hovering the window expands back to the full pill. ----
        Rectangle {
            id: sliver
            visible: hud.collapsed
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            width: 48
            height: 8
            radius: 4
            color: Theme.surfaceTop
            border.width: 1
            border.color: Theme.divider
            Rectangle {
                id: sliverDot
                anchors.centerIn: parent
                width: 6; height: 6; radius: 3
                color: Theme.danger
                property real blinkOpacity: 1.0
                opacity: sliver.visible ? blinkOpacity : 1.0
                SequentialAnimation {
                    running: sliver.visible
                    loops: Animation.Infinite
                    NumberAnimation { target: sliverDot; property: "blinkOpacity"; to: 0.25; duration: 650; easing.type: Easing.InOutSine }
                    NumberAnimation { target: sliverDot; property: "blinkOpacity"; to: 1.0; duration: 650; easing.type: Easing.InOutSine }
                }
            }
        }

        // ---- The full pill ----
        Rectangle {
            id: pill
            visible: !hud.collapsed
            width: hud.fullW
            height: hud.fullH
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            radius: hud.isCountdown ? 20 : height / 2
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

            // ---- Countdown phase: a big, clear number + cancel ----
            Item {
                anchors.fill: parent
                visible: hud.isCountdown
                Column {
                    anchors.centerIn: parent
                    spacing: 2
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: Studio.recorderCountdown > 0 ? Studio.recorderCountdown : qsTr("Go")
                        color: Theme.textPrimary
                        font.pixelSize: 60
                        font.weight: Font.Bold
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("Recording starts…")
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontS
                    }
                }
                UIconButton {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 8
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
