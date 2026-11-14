import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

ColumnLayout {
    id: controls
    property var s: backend.selected
    signal editRequested(string itemId)
    property bool showEditButton: true
    // Folding sections in the inspector; laid open in the Edit transform window.
    property bool collapsible: true
    readonly property var alignments: [qsTr("Top left"), qsTr("Top centre"), qsTr("Top right"), qsTr("Centre left"), qsTr("Centre"), qsTr("Centre right"), qsTr("Bottom left"), qsTr("Bottom centre"), qsTr("Bottom right")]
    component Pair: RowLayout {
        Layout.fillWidth: true
        spacing: Theme.s2
    }
    Layout.fillWidth: true
    spacing: Theme.s2
    InspectorSection {
        objectName: "transformSection"
        title: qsTr("Transform")
        key: "inspector.transform"
        collapsible: controls.collapsible
        trailing: ToolButton { objectName: "editTransformButton"; visible: controls.showEditButton; text: qsTr("Edit…")
                               onClicked: controls.editRequested(controls.s.id)
                               ToolTip.text: qsTr("All of this in a larger window") }
        Pair {
            NumField { objectName: "transformPositionX"; label: qsTr("X"); value: controls.s.positionX ?? 0; Layout.fillWidth: true; enabled: !controls.s.placementLocked
                       ToolTip.text: qsTr("Horizontal position of the position anchor, in canvas pixels")
                       onCommitted: (v) => backend.setItemPosition(controls.s.id, v, controls.s.positionY) }
            NumField { objectName: "transformPositionY"; label: qsTr("Y"); value: controls.s.positionY ?? 0; Layout.fillWidth: true; enabled: !controls.s.placementLocked
                       ToolTip.text: qsTr("Vertical position of the position anchor, in canvas pixels")
                       onCommitted: (v) => backend.setItemPosition(controls.s.id, controls.s.positionX, v) }
        }
        Pair {
            NumField { label: qsTr("W"); value: controls.s.width ?? 0; Layout.fillWidth: true; enabled: !controls.s.placementLocked
                       ToolTip.text: qsTr("Width on the canvas, in pixels")
                       onCommitted: (v) => backend.setItemSize(controls.s.id, v, controls.s.height) }
            NumField { label: qsTr("H"); value: controls.s.height ?? 0; Layout.fillWidth: true; enabled: !controls.s.placementLocked
                       ToolTip.text: qsTr("Height on the canvas, in pixels")
                       onCommitted: (v) => backend.setItemSize(controls.s.id, controls.s.width, v) }
        }
        // Labels on the left, the controls in one column on the right.
        Pair {
            Label { text: qsTr("Rotate"); color: Theme.textSecondary; Layout.fillWidth: true }
            NumField { value: controls.s.rotation ?? 0; suffix: "°"; step: 1; Layout.preferredWidth: Theme.s6 * 4
                       ToolTip.text: qsTr("Rotation in degrees, around the Rotate around point")
                       Accessible.name: qsTr("Rotate")
                       enabled: !controls.s.placementLocked; onCommitted: (v) => backend.setItemRotation(controls.s.id, v) }
        }
        Pair {
            Label { text: qsTr("Sizing"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox {
                ToolTip.text: qsTr("How the picture fits its box: free size keeps its own scale; the others fit, fill, stretch or match the box")
                Layout.preferredWidth: Theme.s6 * 4
                enabled: !controls.s.placementLocked
                objectName: "transformBounds"
                model: [qsTr("Free size"), qsTr("Fit in box"), qsTr("Fill box (crop)"), qsTr("Stretch to box"), qsTr("Cover box"), qsTr("Match box width"), qsTr("Match box height"), qsTr("Shrink to fit")]
                readonly property var values: ["none", "fit", "fill", "stretch", "cover", "width", "height", "max-only"]
                currentIndex: Math.max(0, values.indexOf(controls.s.bounds ?? "none"))
                onActivated: backend.setItemBounds(controls.s.id, values[currentIndex])
                Accessible.name: qsTr("Sizing")
            }
        }
        Pair {
            Label { text: qsTr("Position anchor"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox { objectName: "transformPositionAlignment"; Layout.preferredWidth: Theme.s6 * 4; model: controls.alignments
                ToolTip.text: qsTr("Which point of the item X and Y place. Changing it leaves the item where it is")
                currentIndex: controls.s.positionAlignment ?? 0; enabled: !controls.s.placementLocked
                onActivated: backend.setItemAlignment(controls.s.id, "position", currentIndex); Accessible.name: qsTr("Position anchor") }
        }
        Pair {
            Label { text: qsTr("Rotate around"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox { objectName: "transformRotationAlignment"; Layout.preferredWidth: Theme.s6 * 4; model: controls.alignments
                ToolTip.text: qsTr("The point it turns around. Choose the same point as the position anchor to rotate around the position you typed")
                currentIndex: controls.s.rotationAlignment ?? 4; enabled: !controls.s.placementLocked
                onActivated: backend.setItemAlignment(controls.s.id, "rotation", currentIndex); Accessible.name: qsTr("Rotation pivot") }
        }
        Pair {
            visible: (controls.s.bounds ?? "none") !== "none"
            Label { text: qsTr("Content in box"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox { objectName: "transformBoundsAlignment"; Layout.preferredWidth: Theme.s6 * 4; model: controls.alignments
                ToolTip.text: qsTr("Where the picture sits inside its box when it doesn't fill it")
                currentIndex: controls.s.boundsAlignment ?? 4; enabled: !controls.s.placementLocked
                onActivated: backend.setItemAlignment(controls.s.id, "bounds", currentIndex); Accessible.name: qsTr("Content alignment") }
        }
        CheckBox { objectName: "transformClipBounds"; text: qsTr("Clip content to box"); enabled: !controls.s.placementLocked
            ToolTip.text: qsTr("Trim whatever overhangs the box")
            visible: ["cover", "width", "height"].includes(controls.s.bounds ?? "none")
            checked: controls.s.clipToBounds ?? false; onToggled: backend.setItemClipToBounds(controls.s.id, checked) }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.s1
            enabled: !controls.s.placementLocked
            ToolButton { icon.name: "flip-horizontal-2"; checked: controls.s.flipH ?? false
                         onClicked: backend.setItemFlip(controls.s.id, !controls.s.flipH, controls.s.flipV)
                         ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Mirror"); Accessible.name: ToolTip.text }
            ToolButton { icon.name: "flip-vertical-2"; checked: controls.s.flipV ?? false
                         onClicked: backend.setItemFlip(controls.s.id, controls.s.flipH, !controls.s.flipV)
                         ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Flip"); Accessible.name: ToolTip.text }
            ToolButton { icon.name: "maximize-2"; onClicked: backend.fitItem(controls.s.id)
                         ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Fit to canvas"); Accessible.name: ToolTip.text }
            ToolButton { icon.name: "crosshair"; onClicked: backend.centreItem(controls.s.id)
                         ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Centre"); Accessible.name: ToolTip.text }
            ToolButton { icon.name: "rotate-ccw"; onClicked: backend.resetItem(controls.s.id)
                         ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Reset transform"); Accessible.name: ToolTip.text }
        }
    }
    InspectorSection {
        objectName: "cropSection"
        title: qsTr("Crop")
        key: "inspector.crop"
        startOpen: false
        collapsible: controls.collapsible
        Pair {
            NumField { label: qsTr("L"); value: controls.s.cropLeft ?? 0; Layout.fillWidth: true; enabled: !controls.s.placementLocked
                       ToolTip.text: qsTr("Source pixels cut from the left edge. Alt-drag a handle in the preview to crop there")
                       onCommitted: (v) => backend.setItemCrop(controls.s.id, v, controls.s.cropTop, controls.s.cropRight, controls.s.cropBottom) }
            NumField { label: qsTr("T"); value: controls.s.cropTop ?? 0; Layout.fillWidth: true; enabled: !controls.s.placementLocked
                       ToolTip.text: qsTr("Source pixels cut from the top edge")
                       onCommitted: (v) => backend.setItemCrop(controls.s.id, controls.s.cropLeft, v, controls.s.cropRight, controls.s.cropBottom) }
        }
        Pair {
            NumField { label: qsTr("R"); value: controls.s.cropRight ?? 0; Layout.fillWidth: true; enabled: !controls.s.placementLocked
                       ToolTip.text: qsTr("Source pixels cut from the right edge")
                       onCommitted: (v) => backend.setItemCrop(controls.s.id, controls.s.cropLeft, controls.s.cropTop, v, controls.s.cropBottom) }
            NumField { label: qsTr("B"); value: controls.s.cropBottom ?? 0; Layout.fillWidth: true; enabled: !controls.s.placementLocked
                       ToolTip.text: qsTr("Source pixels cut from the bottom edge")
                       onCommitted: (v) => backend.setItemCrop(controls.s.id, controls.s.cropLeft, controls.s.cropTop, controls.s.cropRight, v) }
        }
    }
    InspectorSection {
        objectName: "blendingSection"
        title: qsTr("Blending")
        key: "inspector.blending"
        startOpen: false
        collapsible: controls.collapsible
        Pair {
            Label { text: qsTr("Opacity"); color: Theme.textSecondary; Layout.fillWidth: true }
            // The slider and its value together as wide as the combos below.
            RowLayout {
                Layout.preferredWidth: Theme.s6 * 4
                Layout.fillWidth: false
                spacing: Theme.s1
            Slider {
                ToolTip.text: qsTr("How see-through the item is")
                Layout.fillWidth: true
                from: 0; to: 1
                value: controls.s.opacity ?? 1
                enabled: !controls.s.placementLocked
                onMoved: backend.setItemOpacity(controls.s.id, value, true)
                onPressedChanged: if (!pressed) backend.endGesture()
                Accessible.name: qsTr("Opacity")
            }
            Label { text: Math.round((controls.s.opacity ?? 1) * 100) + "%"; color: Theme.textMuted; font.family: Theme.monoFamily
                    horizontalAlignment: Text.AlignRight; Layout.preferredWidth: Theme.s5 + Theme.s3 }
            }
        }
        Pair {
            Label { text: qsTr("Blend"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox {
                ToolTip.text: qsTr("How the item mixes with what is beneath it")
                Layout.preferredWidth: Theme.s6 * 4
                enabled: !controls.s.placementLocked
                model: [qsTr("Normal"), qsTr("Multiply"), qsTr("Screen"), qsTr("Overlay"), qsTr("Darken"), qsTr("Lighten"),
                        qsTr("Add"), qsTr("Subtract"), qsTr("Difference")]
                readonly property var values: ["normal", "multiply", "screen", "overlay", "darken", "lighten", "add", "subtract", "difference"]
                currentIndex: Math.max(0, values.indexOf(controls.s.blend ?? "normal"))
                onActivated: backend.setItemBlend(controls.s.id, values[currentIndex])
                Accessible.name: qsTr("Blend")
            }
        }
        Pair {
            Label { text: qsTr("Blend space"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox {
                objectName: "itemBlendSpace"
                Layout.preferredWidth: Theme.s6 * 4
                enabled: !controls.s.placementLocked
                model: [qsTr("Linear light"), qsTr("sRGB")]
                readonly property var values: ["linear", "srgb"]
                currentIndex: Math.max(0, values.indexOf(controls.s.blendSpace ?? "linear"))
                onActivated: backend.setItemBlendSpace(controls.s.id, values[currentIndex])
                Accessible.name: qsTr("Blend space")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Choose how opacity mixes this item with the picture beneath it. Linear light gives brighter translucent edges; sRGB mixes display colour values.")
            }
        }
        Pair {
            Label { text: qsTr("Scaling"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox {
                ToolTip.text: qsTr("How the picture is resized. Auto picks a filter for the change in size; Point keeps pixels crisp for pixel art")
                objectName: "itemScaleFilter"
                Layout.preferredWidth: Theme.s6 * 4
                enabled: !controls.s.placementLocked
                model: [qsTr("Auto"), qsTr("Lanczos (sharpest)"), qsTr("Bicubic"), qsTr("Bilinear"), qsTr("Area (shrinking)"), qsTr("Point (nearest)")]
                readonly property var values: ["auto", "lanczos", "bicubic", "bilinear", "area", "point"]
                currentIndex: Math.max(0, values.indexOf(controls.s.scale ?? "auto"))
                onActivated: backend.setItemScaleFilter(controls.s.id, values[currentIndex])
                Accessible.name: qsTr("Scaling")
            }
        }
    }
}
