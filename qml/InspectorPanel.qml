import QtQuick
import Unisic.Kit

// The editor's right-hand style panel. Every control binds DIRECTLY to the
// C++ StyleModel (two-way through its NOTIFY signals), so edits update the live
// CompositionRoot preview immediately and mark the project dirty. Colors come
// from Theme tokens; controls are kit components — nothing styled is invented
// here. Every label is qsTr()'d.
Rectangle {
    id: panel
    color: Theme.background

    // The C++ StyleModel (editorProject.style). `var` because it isn't a QML type.
    property var styleModel: null
    readonly property var sm: styleModel

    // Selected zoom keyframe (from the timeline / canvas); -1 == none. The
    // keyframe section binds to it; deselect() asks the window to clear it.
    property int selectedKeyframe: -1
    signal deselect()

    // Bump on any zoom-model mutation so the keyframe accessors re-read.
    property int _rev: 0
    Connections {
        target: (typeof editorProject !== "undefined" && editorProject) ? editorProject.zoom : null
        ignoreUnknownSignals: true
        function onChanged() { panel._rev = panel._rev + 1 }
    }
    readonly property var kf: (selectedKeyframe >= 0 && _rev >= 0
                               && typeof editorProject !== "undefined" && editorProject)
                              ? editorProject.zoom.keyframeAt(selectedKeyframe) : null
    readonly property bool hasKf: kf !== null && kf !== undefined && kf.tMs !== undefined
    readonly property real kfZoom: hasKf ? Studio.zoomFactorOf(editorProject, selectedKeyframe) : 1
    readonly property var _easePresets: [300, 650, 900, 1200]

    // Click on empty inspector space clears the keyframe selection (kit controls
    // grab their own taps, so this only fires on the background).
    TapHandler { onTapped: panel.deselect() }

    // Index maps between the model's string enums and the combo rows.
    readonly property var _bgTypes: ["color", "gradient", "wallpaper", "desktopBlur"]
    readonly property var _frames: ["none", "minimal", "titlebar"]
    readonly property var _webcamPositions: ["bottomRight", "bottomLeft", "topRight", "topLeft"]

    // Whether the loaded project actually recorded a webcam sidecar.
    readonly property bool hasWebcam: (typeof editorProject !== "undefined" && editorProject)
                                      ? editorProject.hasWebcam : false

    // Curated gradient presets (pure colour pairs — NO binary assets). First is
    // the kit Primary→Tertiary default. Clicking a swatch sets both stops.
    readonly property var _gradientPresets: [
        { a: "#17153B", b: "#433D8B" },  // Kit (Primary → Tertiary)
        { a: "#0F2027", b: "#2C5364" },  // Midnight
        { a: "#FF512F", b: "#DD2476" },  // Sunset
        { a: "#2193B0", b: "#6DD5ED" },  // Ocean
        { a: "#134E5E", b: "#71B280" },  // Forest
        { a: "#4A00E0", b: "#8E2DE2" },  // Grape
        { a: "#DE6262", b: "#FFB88C" },  // Peach
        { a: "#232526", b: "#414345" }   // Slate
    ]

    // ---- Reusable rows ------------------------------------------------------
    component LabeledSlider: Column {
        property string label: ""
        property real from: 0
        property real to: 100
        property real value: 0
        property real stepSize: 1
        property int decimals: 0
        property string suffix: ""
        signal moved(real v)
        width: parent ? parent.width : 0
        spacing: 4
        Row {
            width: parent.width
            Text {
                text: label
                color: Theme.textSecondary
                font.pixelSize: Theme.fontS
                width: parent.width - valLabel.width
                elide: Text.ElideRight
            }
            Text {
                id: valLabel
                text: value.toFixed(decimals) + suffix
                color: Theme.textTertiary
                font.pixelSize: Theme.fontS
            }
        }
        USlider {
            width: parent.width
            from: parent.from
            to: parent.to
            value: parent.value
            stepSize: parent.stepSize
            onMoved: (v) => parent.moved(v)
        }
    }

    component LabeledCombo: Column {
        property string label: ""
        property var model: []
        property int currentIndex: 0
        signal activated(int i)
        width: parent ? parent.width : 0
        spacing: 4
        Text { text: parent.label; color: Theme.textSecondary; font.pixelSize: Theme.fontS }
        UComboBox {
            width: parent.width
            model: parent.model
            currentIndex: parent.currentIndex
            onActivated: (i) => parent.activated(i)
        }
    }

    component ColorRow: Row {
        property string label: ""
        property color color: "black"
        property bool showAlpha: false
        signal picked(color c)
        width: parent ? parent.width : 0
        spacing: Theme.spacingM
        Text {
            text: parent.label
            color: Theme.textSecondary
            font.pixelSize: Theme.fontS
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - dot.width - parent.spacing
            elide: Text.ElideRight
        }
        ColorDot { id: dot; dotColor: parent.color; onClicked: pop.openWith(parent.color) }
        UColorPopup { id: pop; showAlpha: parent.showAlpha; onPicked: (c) => parent.picked(c) }
    }

    // ---- Scrollable content -------------------------------------------------
    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: Theme.spacingL
        contentHeight: col.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        MiddleScroll { flickable: flick }
        WheelBoost { flickable: flick }

        Column {
            id: col
            width: parent.width
            spacing: Theme.spacingL

            Text {
                text: qsTr("Style")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontL
                font.weight: Font.Bold
            }

            // ---- Selected zoom keyframe --------------------------------------
            UCard {
                width: parent.width
                visible: panel.hasKf
                Column {
                    width: parent.width
                    spacing: Theme.spacingM

                    Row {
                        width: parent.width
                        Text {
                            text: qsTr("Zoom keyframe")
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontM
                            font.weight: Font.DemiBold
                            width: parent.width - closeBtn.width
                            elide: Text.ElideRight
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        UIconButton {
                            id: closeBtn
                            iconName: "window-close"
                            iconSize: 13; width: 26; height: 26
                            tooltip: qsTr("Deselect")
                            onClicked: panel.deselect()
                        }
                    }

                    LabeledSlider {
                        label: qsTr("Zoom")
                        from: 1.0; to: 3.0; stepSize: 0.05; decimals: 2; suffix: "×"
                        value: panel.kfZoom
                        onMoved: (v) => {
                            if (panel.hasKf)
                                Studio.setZoomFactor(editorProject, panel.selectedKeyframe, v)
                        }
                    }

                    // Position nudge pad (moves the camera centre by 2%).
                    Column {
                        width: parent.width
                        spacing: 4
                        Text {
                            text: qsTr("Position")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontS
                        }
                        Grid {
                            anchors.horizontalCenter: parent.horizontalCenter
                            columns: 3
                            spacing: 2
                            Item { width: 30; height: 30 }
                            UIconButton {
                                iconName: "arrow-up"; iconSize: 14; width: 30; height: 30
                                onClicked: if (panel.hasKf) Studio.nudgeZoom(editorProject, panel.selectedKeyframe, 0, -0.02)
                            }
                            Item { width: 30; height: 30 }
                            UIconButton {
                                iconName: "arrow-left"; iconSize: 14; width: 30; height: 30
                                onClicked: if (panel.hasKf) Studio.nudgeZoom(editorProject, panel.selectedKeyframe, -0.02, 0)
                            }
                            Item { width: 30; height: 30 }
                            UIconButton {
                                iconName: "arrow-right"; iconSize: 14; width: 30; height: 30
                                onClicked: if (panel.hasKf) Studio.nudgeZoom(editorProject, panel.selectedKeyframe, 0.02, 0)
                            }
                            Item { width: 30; height: 30 }
                            UIconButton {
                                iconName: "arrow-down"; iconSize: 14; width: 30; height: 30
                                onClicked: if (panel.hasKf) Studio.nudgeZoom(editorProject, panel.selectedKeyframe, 0, 0.02)
                            }
                            Item { width: 30; height: 30 }
                        }
                    }

                    // Easing presets (ms).
                    Row {
                        width: parent.width
                        spacing: Theme.spacingM
                        Column {
                            width: (parent.width - parent.spacing) / 2
                            spacing: 4
                            Text { text: qsTr("Ease in"); color: Theme.textSecondary; font.pixelSize: Theme.fontS }
                            UValueCombo {
                                width: parent.width
                                values: panel._easePresets
                                from: 0; to: 4000; suffix: " ms"
                                value: panel.hasKf ? panel.kf.easeInMs : 650
                                onChanged: (v) => {
                                    if (panel.hasKf)
                                        editorProject.zoom.setKeyframeEasing(panel.selectedKeyframe, v, panel.kf.easeOutMs)
                                }
                            }
                        }
                        Column {
                            width: (parent.width - parent.spacing) / 2
                            spacing: 4
                            Text { text: qsTr("Ease out"); color: Theme.textSecondary; font.pixelSize: Theme.fontS }
                            UValueCombo {
                                width: parent.width
                                values: panel._easePresets
                                from: 0; to: 4000; suffix: " ms"
                                value: panel.hasKf ? panel.kf.easeOutMs : 900
                                onChanged: (v) => {
                                    if (panel.hasKf)
                                        editorProject.zoom.setKeyframeEasing(panel.selectedKeyframe, panel.kf.easeInMs, v)
                                }
                            }
                        }
                    }

                    // Lock + delete.
                    Row {
                        width: parent.width
                        spacing: Theme.spacingM
                        Text {
                            text: qsTr("Locked")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontS
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - lockSwitch.width - delBtn.width - 2 * parent.spacing
                            elide: Text.ElideRight
                        }
                        USwitch {
                            id: lockSwitch
                            anchors.verticalCenter: parent.verticalCenter
                            checked: panel.hasKf ? panel.kf.locked : false
                            onToggled: (c) => {
                                if (panel.hasKf)
                                    editorProject.zoom.setKeyframeLocked(panel.selectedKeyframe, c)
                            }
                        }
                        UButton {
                            id: delBtn
                            variant: "danger"
                            compact: true
                            text: qsTr("Delete")
                            anchors.verticalCenter: parent.verticalCenter
                            onClicked: {
                                if (panel.hasKf) {
                                    var i = panel.selectedKeyframe
                                    panel.deselect()
                                    editorProject.zoom.removeAt(i)
                                }
                            }
                        }
                    }
                }
            }

            // ---- Background ----
            UCard {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: Theme.spacingM
                    Text {
                        text: qsTr("Background")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontM
                        font.weight: Font.DemiBold
                    }
                    LabeledCombo {
                        label: qsTr("Type")
                        model: [qsTr("Color"), qsTr("Gradient"), qsTr("Wallpaper"), qsTr("Desktop blur")]
                        currentIndex: sm ? Math.max(0, panel._bgTypes.indexOf(sm.backgroundType)) : 0
                        onActivated: (i) => { if (sm) sm.backgroundType = panel._bgTypes[i] }
                    }
                    ColorRow {
                        visible: sm && sm.backgroundType === "color"
                        label: qsTr("Fill")
                        color: sm ? sm.backgroundColor : "black"
                        onPicked: (c) => { if (sm) sm.backgroundColor = c }
                    }
                    ColorRow {
                        visible: sm && sm.backgroundType === "gradient"
                        label: qsTr("Top")
                        color: sm ? sm.gradientStart : "black"
                        onPicked: (c) => { if (sm) sm.gradientStart = c }
                    }
                    ColorRow {
                        visible: sm && sm.backgroundType === "gradient"
                        label: qsTr("Bottom")
                        color: sm ? sm.gradientEnd : "black"
                        onPicked: (c) => { if (sm) sm.gradientEnd = c }
                    }

                    // Gradient presets — clickable swatches (pure QML gradients).
                    Column {
                        visible: sm && sm.backgroundType === "gradient"
                        width: parent.width
                        spacing: 4
                        Text {
                            text: qsTr("Presets")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontS
                        }
                        Flow {
                            width: parent.width
                            spacing: Theme.spacingS
                            Repeater {
                                model: panel._gradientPresets
                                Rectangle {
                                    width: 44
                                    height: 30
                                    radius: Theme.radiusS
                                    border.width: 1
                                    border.color: Theme.divider
                                    gradient: Gradient {
                                        GradientStop { position: 0.0; color: modelData.a }
                                        GradientStop { position: 1.0; color: modelData.b }
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            if (sm) {
                                                sm.gradientStart = modelData.a
                                                sm.gradientEnd = modelData.b
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // desktopBlur is a blurred copy of the recording's first
                    // frame; state the approximation honestly in the UI.
                    Text {
                        visible: sm && sm.backgroundType === "desktopBlur"
                        width: parent.width
                        text: qsTr("Uses a blurred copy of the video's first frame.")
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontS
                        wrapMode: Text.WordWrap
                    }
                    Row {
                        visible: sm && sm.backgroundType === "wallpaper"
                        width: parent.width
                        spacing: Theme.spacingS
                        UTextField {
                            id: wallField
                            width: parent.width - browseBtn.width - parent.spacing
                            placeholder: qsTr("No image selected")
                            text: sm ? sm.wallpaperPath : ""
                            onAccepted: { if (sm) sm.wallpaperPath = text }
                        }
                        UButton {
                            id: browseBtn
                            text: qsTr("Browse…")
                            variant: "tonal"
                            onClicked: {
                                const p = Studio.pickWallpaper(sm ? sm.wallpaperPath : "")
                                if (p !== "" && sm) sm.wallpaperPath = p
                            }
                        }
                    }
                }
            }

            // ---- Layout ----
            UCard {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: Theme.spacingM
                    Text {
                        text: qsTr("Layout")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontM
                        font.weight: Font.DemiBold
                    }
                    LabeledSlider {
                        label: qsTr("Padding")
                        from: 0; to: 30; suffix: "%"
                        value: sm ? sm.paddingPct : 0
                        onMoved: (v) => { if (sm) sm.paddingPct = v }
                    }
                    LabeledSlider {
                        label: qsTr("Corner radius")
                        from: 0; to: 40
                        value: sm ? sm.cornerRadius : 0
                        onMoved: (v) => { if (sm) sm.cornerRadius = Math.round(v) }
                    }
                    // Crop-to-fill vs letterbox. Changing it re-frames the auto camera
                    // (manual/locked keyframes are kept), so regenerate on change.
                    LabeledCombo {
                        label: qsTr("Fill mode")
                        model: [qsTr("Crop to fill"), qsTr("Fit (letterbox)")]
                        currentIndex: (sm && sm.fillMode === "fit") ? 1 : 0
                        onActivated: (i) => {
                            if (!sm) return
                            sm.fillMode = i === 1 ? "fit" : "fill"
                            if (typeof editorProject !== "undefined" && editorProject)
                                Studio.regenerateZoom(editorProject)
                        }
                    }
                }
            }

            // ---- Cursor ----
            UCard {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: Theme.spacingM
                    Text {
                        text: qsTr("Cursor")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontM
                        font.weight: Font.DemiBold
                    }
                    LabeledCombo {
                        label: qsTr("Style")
                        model: [qsTr("Pointer"), qsTr("System"), qsTr("Dot"), qsTr("Circle")]
                        readonly property var ids: ["pointer", "system", "dot", "circle"]
                        currentIndex: sm ? Math.max(0, ids.indexOf(sm.cursorStyle)) : 0
                        onActivated: (i) => { if (sm) sm.cursorStyle = ids[i] }
                    }
                    LabeledSlider {
                        label: qsTr("Size")
                        from: 0.5; to: 3.0; stepSize: 0.1; decimals: 1; suffix: "×"
                        value: sm ? sm.cursorScale : 1.6
                        onMoved: (v) => { if (sm) sm.cursorScale = v }
                    }
                    Row {
                        width: parent.width
                        Text {
                            text: qsTr("Click ripple")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontS
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - rippleSwitch.width
                            elide: Text.ElideRight
                        }
                        USwitch {
                            id: rippleSwitch
                            checked: sm ? sm.clickRipple : true
                            onToggled: (c) => { if (sm) sm.clickRipple = c }
                        }
                    }
                    ColorRow {
                        visible: sm && sm.clickRipple
                        label: qsTr("Ripple color")
                        color: sm ? sm.rippleColor : "#C8ACD6"
                        onPicked: (c) => { if (sm) sm.rippleColor = c }
                    }
                }
            }

            // ---- Shadow ----
            UCard {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: Theme.spacingM
                    Text {
                        text: qsTr("Shadow")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontM
                        font.weight: Font.DemiBold
                    }
                    LabeledSlider {
                        label: qsTr("Blur")
                        from: 0; to: 100
                        value: sm ? sm.shadowBlur : 0
                        onMoved: (v) => { if (sm) sm.shadowBlur = Math.round(v) }
                    }
                    LabeledSlider {
                        label: qsTr("Opacity")
                        from: 0; to: 1; stepSize: 0.05; decimals: 2
                        value: sm ? sm.shadowOpacity : 0
                        onMoved: (v) => { if (sm) sm.shadowOpacity = v }
                    }
                    LabeledSlider {
                        label: qsTr("Offset Y")
                        from: 0; to: 40
                        value: sm ? sm.shadowOffsetY : 0
                        onMoved: (v) => { if (sm) sm.shadowOffsetY = Math.round(v) }
                    }
                }
            }

            // ---- Frame ----
            UCard {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: Theme.spacingM
                    Text {
                        text: qsTr("Frame")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontM
                        font.weight: Font.DemiBold
                    }
                    LabeledCombo {
                        label: qsTr("Style")
                        model: [qsTr("None"), qsTr("Minimal"), qsTr("Title bar")]
                        currentIndex: sm ? Math.max(0, panel._frames.indexOf(sm.frameStyle)) : 0
                        onActivated: (i) => { if (sm) sm.frameStyle = panel._frames[i] }
                    }
                    Column {
                        visible: sm && sm.frameStyle === "titlebar"
                        width: parent.width
                        spacing: 4
                        Text {
                            text: qsTr("Title")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontS
                        }
                        UTextField {
                            width: parent.width
                            placeholder: qsTr("Window title")
                            text: sm ? sm.frameTitle : ""
                            onEdited: (t) => { if (sm) sm.frameTitle = t }
                        }
                    }
                }
            }

            // ---- Webcam ----
            UCard {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: Theme.spacingM
                    Text {
                        text: qsTr("Webcam")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontM
                        font.weight: Font.DemiBold
                    }
                    // No sidecar recorded → explain how to get one.
                    Text {
                        visible: !panel.hasWebcam
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: qsTr("This project has no webcam recording. Enable “Record webcam” in Settings before recording.")
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontS
                    }
                    Row {
                        visible: panel.hasWebcam
                        width: parent.width
                        Text {
                            text: qsTr("Show webcam")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontS
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - wcSwitch.width
                            elide: Text.ElideRight
                        }
                        USwitch {
                            id: wcSwitch
                            checked: sm ? sm.webcamEnabled : false
                            onToggled: (c) => { if (sm) sm.webcamEnabled = c }
                        }
                    }
                    LabeledCombo {
                        visible: panel.hasWebcam && sm && sm.webcamEnabled
                        label: qsTr("Position")
                        model: [qsTr("Bottom right"), qsTr("Bottom left"), qsTr("Top right"), qsTr("Top left")]
                        currentIndex: sm ? Math.max(0, panel._webcamPositions.indexOf(sm.webcamPosition)) : 0
                        onActivated: (i) => { if (sm) sm.webcamPosition = panel._webcamPositions[i] }
                    }
                    LabeledSlider {
                        visible: panel.hasWebcam && sm && sm.webcamEnabled
                        label: qsTr("Size")
                        from: 8; to: 40; suffix: "%"
                        value: sm ? sm.webcamSizePct : 20
                        onMoved: (v) => { if (sm) sm.webcamSizePct = v }
                    }
                    Row {
                        visible: panel.hasWebcam && sm && sm.webcamEnabled
                        width: parent.width
                        Text {
                            text: qsTr("Circle")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontS
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - roundSwitch.width
                            elide: Text.ElideRight
                        }
                        USwitch {
                            id: roundSwitch
                            checked: sm ? sm.webcamRounded : true
                            onToggled: (c) => { if (sm) sm.webcamRounded = c }
                        }
                    }
                }
            }
        }
    }
}
