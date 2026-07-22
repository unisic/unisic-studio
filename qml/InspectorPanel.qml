import QtQuick
import Unisic.Kit

// The editor's right-hand style panel. Every control binds DIRECTLY to the
// C++ StyleModel (two-way through its NOTIFY signals), so edits update the live
// CompositionRoot preview immediately and mark the project dirty. Colors come
// from Theme tokens; controls are kit components — nothing styled is invented
// here. Every label is qsTr()'d.
//
// Layout: a scrollable stack of always-visible, collapsible section cards
// (Background / Layout / Cursor / Shadow / Frame / Webcam), each with an icon +
// chevron header. When a zoom keyframe is selected its card is pushed ON TOP of
// the stack (not replacing the style cards) so the style panel is never lost.
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
    // USlider emits `moved` on press and on every drag step, so a direct
    // regenerate would run a full synchronous KeyframeEngine::generate() ~21
    // times per drag (stepSize 0.05 over 0..1) on the GUI thread. The slider's
    // value binding stays live; only the regenerate is coalesced.
    Timer {
        id: zoomIntensityDebounce
        interval: 240
        repeat: false
        onTriggered: {
            if (typeof editorProject !== "undefined" && editorProject)
                Studio.regenerateZoom(editorProject)
        }
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
    readonly property var _bgTypes: ["none", "color", "gradient", "wallpaper", "desktopBlur"]
    readonly property var _frames: ["none", "minimal", "titlebar"]
    readonly property var _webcamPositions: ["bottomRight", "bottomLeft", "topRight", "topLeft"]
    readonly property var _cursorStyles: ["pointer", "system", "dot", "circle"]

    // Named motion presets (Screen Studio-style "animation styles"): each chip
    // writes BOTH tuning values, so `checked` is honest conjunctive equality —
    // lit only when intensity AND smoothness both match (same pattern as
    // ExportDialog's quality presets). "Balanced" mirrors the shipped defaults.
    readonly property var _motionPresets: [
        { label: qsTr("Gentle"),   intensity: 0.45, smoothness: 0.85 },
        { label: qsTr("Balanced"), intensity: 0.72, smoothness: 0.68 },
        { label: qsTr("Snappy"),   intensity: 0.85, smoothness: 0.45 },
        { label: qsTr("Rapid"),    intensity: 1.0,  smoothness: 0.30 }
    ]
    function motionPresetChecked(p) {
        if (typeof editorProject === "undefined" || !editorProject) return false
        return Math.abs(editorProject.zoom.zoomIntensity - p.intensity) < 0.01
            && Math.abs(editorProject.zoom.motionSmoothness - p.smoothness) < 0.01
    }
    // No chip matches → the user drove the sliders directly; show a quiet
    // "Custom" hint instead of an unlit row that reads as "nothing selected".
    readonly property bool customMotionPreset: {
        for (var i = 0; i < _motionPresets.length; ++i)
            if (motionPresetChecked(_motionPresets[i]))
                return false
        return true
    }

    // Whether the loaded project actually recorded a webcam sidecar.
    readonly property bool hasWebcam: (typeof editorProject !== "undefined" && editorProject)
                                      ? editorProject.hasWebcam : false

    // Clip-audio state (PROJECT-level, not StyleModel — see StudioProject).
    readonly property bool audioMuted: (typeof editorProject !== "undefined" && editorProject)
                                       ? editorProject.audioMuted : false

    // Curated gradient presets (pure colour pairs — NO binary assets). First is
    // the muted kit Primary→Secondary default. Clicking a swatch sets both stops.
    readonly property var _gradientPresets: [
        { a: "#17153B", b: "#2E236C" },  // Kit (Primary -> Secondary)
        { a: "#0F2027", b: "#2C5364" },  // Midnight
        { a: "#FF512F", b: "#DD2476" },  // Sunset
        { a: "#2193B0", b: "#6DD5ED" },  // Ocean
        { a: "#134E5E", b: "#71B280" },  // Forest
        { a: "#4A00E0", b: "#8E2DE2" },  // Grape
        { a: "#DE6262", b: "#FFB88C" },  // Peach
        { a: "#232526", b: "#414345" },  // Slate
        { a: "#F12711", b: "#F5AF19" },  // Ember
        { a: "#00C9FF", b: "#92FE9D" },  // Aurora
        { a: "#C31432", b: "#240B36" },  // Berry
        { a: "#485563", b: "#29323C" },  // Steel
        { a: "#654EA3", b: "#EAAFC8" },  // Lavender
        { a: "#141E30", b: "#243B55" },  // Coal (dark neutral)
        { a: "#3E5151", b: "#DECBA4" },  // Sandstone
        { a: "#8E9EAB", b: "#EEF2F3" }   // Ash (light neutral)
    ]

    // Flat single-colour presets (sets backgroundColor).
    readonly property var _flatPresets: ["#1E1E24", "#17153B", "#2E236C", "#ECECF2"]

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

    // Always-visible, clickable section header with an icon + a collapse chevron.
    // Owns its own `expanded` state (default open); the body binds visibility to it.
    component SectionHeader: Item {
        id: hdr
        property string title: ""
        property string iconName: ""
        property bool expanded: true
        width: parent ? parent.width : 0
        height: 24
        Row {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacingS
            UIcon {
                visible: hdr.iconName !== ""
                anchors.verticalCenter: parent.verticalCenter
                name: hdr.iconName
                size: 15
                color: Theme.textSecondary
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: hdr.title
                color: Theme.textPrimary
                font.pixelSize: Theme.fontM
                font.weight: Font.DemiBold
            }
        }
        UIcon {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            name: hdr.expanded ? "arrow-down" : "arrow-right"
            size: 13
            color: Theme.textTertiary
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: hdr.expanded = !hdr.expanded
        }
    }

    // A cursor-style preview swatch (tiny motif + label); highlighted when active.
    component CursorTile: Column {
        id: tile
        property string sid: ""
        property string label: ""
        property bool active: false
        signal picked()
        width: 58
        spacing: 3
        Rectangle {
            width: parent.width
            height: 40
            radius: Theme.radiusS
            color: tile.active ? Theme.surfaceHi : Theme.surface
            border.width: tile.active ? 2 : 1
            border.color: tile.active ? Theme.accent : Theme.divider
            // Pointer: the shipped cursor asset itself (same qrc URL the
            // composition renders), so the tile previews the real thing.
            Image {
                anchors.centerIn: parent
                visible: tile.sid === "pointer"
                source: "qrc:/resources/cursors/pointer.svg"
                width: 15
                height: 20   // pointer.svg viewBox is 24×32 (3:4)
                sourceSize: Qt.size(30, 40)   // 2× for crisp SVG rasterisation
                smooth: true
            }
            // System: the recorded mouse glyph.
            UIcon {
                anchors.centerIn: parent
                visible: tile.sid === "system"
                name: "input-mouse"
                size: 20
                color: Theme.textPrimary
            }
            // Dot: a filled disc.
            Rectangle {
                anchors.centerIn: parent
                visible: tile.sid === "dot"
                width: 12; height: 12; radius: 6
                color: Theme.textPrimary
            }
            // Circle: a ring.
            Rectangle {
                anchors.centerIn: parent
                visible: tile.sid === "circle"
                width: 18; height: 18; radius: 9
                color: "transparent"
                border.width: 2
                border.color: Theme.textPrimary
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: tile.picked()
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: tile.label
            color: tile.active ? Theme.textPrimary : Theme.textSecondary
            font.pixelSize: Theme.fontS
        }
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

            // ---- Selected zoom keyframe (pushed on top of the stack) ---------
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

            // ---- Global camera motion ----------------------------------------
            UCard {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: motionHeader.expanded ? Theme.spacingM : 0
                    SectionHeader {
                        id: motionHeader
                        title: qsTr("Motion")
                        iconName: "media-playback-start"
                    }
                    Column {
                        visible: motionHeader.expanded
                        width: parent.width
                        spacing: Theme.spacingM
                        // Preset chips are shortcuts over the two sliders below,
                        // not a replacement. They coalesce the regenerate
                        // through the same debounce as the intensity slider — a
                        // direct Studio.regenerateZoom here would bypass it.
                        Flow {
                            width: parent.width
                            spacing: Theme.spacingS
                            Repeater {
                                model: panel._motionPresets
                                UFilterChip {
                                    required property var modelData
                                    text: modelData.label
                                    checked: panel.motionPresetChecked(modelData)
                                    onClicked: {
                                        if (typeof editorProject === "undefined" || !editorProject)
                                            return
                                        editorProject.zoom.zoomIntensity = modelData.intensity
                                        editorProject.zoom.motionSmoothness = modelData.smoothness
                                        zoomIntensityDebounce.restart()
                                    }
                                }
                            }
                        }
                        Text {
                            width: parent.width
                            visible: panel.customMotionPreset
                            text: qsTr("Custom — tuned with the sliders below.")
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontS
                            wrapMode: Text.WordWrap
                        }
                        LabeledSlider {
                            label: qsTr("Zoom intensity")
                            from: 0; to: 1; stepSize: 0.05; decimals: 2
                            value: (typeof editorProject !== "undefined" && editorProject)
                                   ? editorProject.zoom.zoomIntensity : 0.72
                            onMoved: (v) => {
                                if (typeof editorProject === "undefined" || !editorProject) return
                                editorProject.zoom.zoomIntensity = v
                                zoomIntensityDebounce.restart()
                            }
                        }
                        LabeledSlider {
                            label: qsTr("Motion smoothness")
                            from: 0; to: 1; stepSize: 0.05; decimals: 2
                            value: (typeof editorProject !== "undefined" && editorProject)
                                   ? editorProject.zoom.motionSmoothness : 0.68
                            onMoved: (v) => {
                                if (typeof editorProject !== "undefined" && editorProject)
                                    editorProject.zoom.motionSmoothness = v
                            }
                        }
                        Text {
                            width: parent.width
                            text: qsTr("Intensity changes framing and frequency. Smoothness changes spring response.")
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontS
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            // ---- Video (clip audio) ------------------------------------------
            UCard {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: videoHeader.expanded ? Theme.spacingM : 0
                    SectionHeader {
                        id: videoHeader
                        title: qsTr("Video")
                        iconName: "video-x-generic"
                    }
                    Column {
                        visible: videoHeader.expanded
                        width: parent.width
                        spacing: Theme.spacingM
                        // Project stores linear gain 0..1; the slider shows %.
                        LabeledSlider {
                            enabled: !panel.audioMuted
                            opacity: enabled ? 1 : 0.4
                            label: qsTr("Volume")
                            from: 0; to: 100; stepSize: 1; suffix: "%"
                            value: (typeof editorProject !== "undefined" && editorProject)
                                   ? editorProject.audioVolume * 100 : 100
                            onMoved: (v) => {
                                if (typeof editorProject !== "undefined" && editorProject)
                                    editorProject.audioVolume = v / 100
                            }
                        }
                        Row {
                            width: parent.width
                            Text {
                                text: qsTr("Mute")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontS
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - muteSwitch.width
                                elide: Text.ElideRight
                            }
                            USwitch {
                                id: muteSwitch
                                checked: panel.audioMuted
                                onToggled: (c) => {
                                    if (typeof editorProject !== "undefined" && editorProject)
                                        editorProject.audioMuted = c
                                }
                            }
                        }
                    }
                }
            }

            // ---- Background --------------------------------------------------
            UCard {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: bgHeader.expanded ? Theme.spacingM : 0
                    SectionHeader { id: bgHeader; title: qsTr("Background"); iconName: "image" }
                    Column {
                        visible: bgHeader.expanded
                        width: parent.width
                        spacing: Theme.spacingM

                        LabeledCombo {
                            label: qsTr("Type")
                            model: [qsTr("None"), qsTr("Color"), qsTr("Gradient"), qsTr("Wallpaper"), qsTr("Desktop blur")]
                            currentIndex: sm ? Math.max(0, panel._bgTypes.indexOf(sm.backgroundType)) : 0
                            onActivated: (i) => { if (sm) sm.backgroundType = panel._bgTypes[i] }
                        }
                        ColorRow {
                            visible: sm && sm.backgroundType === "color"
                            label: qsTr("Fill")
                            color: sm ? sm.backgroundColor : "black"
                            onPicked: (c) => { if (sm) sm.backgroundColor = c }
                        }

                        // Flat-colour presets (clickable swatches).
                        Column {
                            visible: sm && sm.backgroundType === "color"
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
                                    model: panel._flatPresets
                                    Rectangle {
                                        required property var modelData
                                        width: 44
                                        height: 30
                                        radius: Theme.radiusS
                                        border.width: 1
                                        border.color: Theme.divider
                                        color: modelData
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: { if (sm) sm.backgroundColor = modelData }
                                        }
                                    }
                                }
                            }
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
                                        required property var modelData
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
            }

            // ---- Layout ------------------------------------------------------
            UCard {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: layoutHeader.expanded ? Theme.spacingM : 0
                    SectionHeader { id: layoutHeader; title: qsTr("Layout"); iconName: "transform-crop" }
                    Column {
                        visible: layoutHeader.expanded
                        width: parent.width
                        spacing: Theme.spacingM
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
            }

            // ---- Cursor ------------------------------------------------------
            UCard {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: cursorHeader.expanded ? Theme.spacingM : 0
                    SectionHeader { id: cursorHeader; title: qsTr("Cursor"); iconName: "input-mouse" }
                    Column {
                        visible: cursorHeader.expanded
                        width: parent.width
                        spacing: Theme.spacingM

                        // Style — segmented preview tiles.
                        Column {
                            width: parent.width
                            spacing: 4
                            Text { text: qsTr("Style"); color: Theme.textSecondary; font.pixelSize: Theme.fontS }
                            Row {
                                width: parent.width
                                spacing: Theme.spacingS
                                CursorTile {
                                    sid: "pointer"; label: qsTr("Pointer")
                                    active: sm && sm.cursorStyle === "pointer"
                                    onPicked: { if (sm) sm.cursorStyle = "pointer" }
                                }
                                CursorTile {
                                    sid: "system"; label: qsTr("System")
                                    active: sm && sm.cursorStyle === "system"
                                    onPicked: { if (sm) sm.cursorStyle = "system" }
                                }
                                CursorTile {
                                    sid: "dot"; label: qsTr("Highlight")
                                    active: sm && sm.cursorStyle === "dot"
                                    onPicked: { if (sm) sm.cursorStyle = "dot" }
                                }
                                CursorTile {
                                    sid: "circle"; label: qsTr("Precision")
                                    active: sm && sm.cursorStyle === "circle"
                                    onPicked: { if (sm) sm.cursorStyle = "circle" }
                                }
                            }
                        }

                        LabeledSlider {
                            label: qsTr("Size")
                            from: 1.0; to: 3.0; stepSize: 0.1; decimals: 1; suffix: "×"
                            value: sm ? sm.cursorScale : 1.65
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
                        Text {
                            width: parent.width
                            text: qsTr("Cursor hides after idle. Clicks add a soft ring and tactile press.")
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontS
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            // ---- Shadow ------------------------------------------------------
            UCard {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: shadowHeader.expanded ? Theme.spacingM : 0
                    SectionHeader { id: shadowHeader; title: qsTr("Shadow"); iconName: "edit-image" }
                    Column {
                        visible: shadowHeader.expanded
                        width: parent.width
                        spacing: Theme.spacingM
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
            }

            // ---- Frame -------------------------------------------------------
            UCard {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: frameHeader.expanded ? Theme.spacingM : 0
                    SectionHeader { id: frameHeader; title: qsTr("Frame"); iconName: "window" }
                    Column {
                        visible: frameHeader.expanded
                        width: parent.width
                        spacing: Theme.spacingM
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
            }

            // ---- Webcam ------------------------------------------------------
            UCard {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: webcamHeader.expanded ? Theme.spacingM : 0
                    SectionHeader { id: webcamHeader; title: qsTr("Webcam"); iconName: "camera-web" }
                    Column {
                        visible: webcamHeader.expanded
                        width: parent.width
                        spacing: Theme.spacingM
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
}
