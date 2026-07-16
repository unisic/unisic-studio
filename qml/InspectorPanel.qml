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

    // Index maps between the model's string enums and the combo rows.
    readonly property var _bgTypes: ["color", "gradient", "wallpaper"]
    readonly property var _frames: ["none", "minimal", "titlebar"]

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
                        model: [qsTr("Color"), qsTr("Gradient"), qsTr("Wallpaper")]
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
        }
    }
}
