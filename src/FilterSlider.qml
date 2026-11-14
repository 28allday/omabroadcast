import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// One number in a filter: its name, a slider, what it is set to, and back to
// where it started. The sheet owns the value: this only asks for changes,
// `dragging` true while a drag goes on, then `finished`. The arrow keys move it
// a step at a time.
RowLayout {
    id: root
    spacing: Theme.s2
    property string label: ""
    property real labelWidth: -1
    property string tip: ""
    property real value: 0
    property real initial: 0
    property real minimum: 0
    property real maximum: 1
    property real step: 1
    property string suffix: ""
    readonly property bool dragging: slider.pressed
    signal committed(real value)
    signal finished()

    // A pixel measure with room for far more than a picture: the slider covers
    // the canvas and whatever is set already, or a pixel would be out of reach.
    readonly property real reach: suffix === "px" && maximum > 2048 ? Math.max(backend.canvasWidth, backend.canvasHeight) : 0
    readonly property real low: reach > 0 && minimum < 0 ? Math.max(minimum, Math.min(-reach, value)) : minimum
    readonly property real high: reach > 0 ? Math.min(maximum, Math.max(reach, value)) : maximum
    readonly property int decimals: step < 0.1 ? 2 : step < 1 ? 1 : 0
    function shown(v) { return Number(v).toFixed(decimals) + (suffix ? " " + suffix : "") }

    Label {
        text: root.label
        color: Theme.textSecondary
        elide: Text.ElideRight
        Layout.preferredWidth: root.labelWidth >= 0 ? root.labelWidth : implicitWidth
    }
    Slider {
        id: slider
        objectName: root.objectName + "_slider"
        Layout.fillWidth: true
        from: root.low; to: root.high
        stepSize: root.step
        snapMode: Slider.SnapAlways
        value: root.value
        onMoved: root.committed(value)
        onPressedChanged: if (!pressed) root.finished()
        ToolTip.text: root.tip || root.label
        Accessible.name: root.label
    }
    TextMetrics { id: widest; font: valueLabel.font; text: root.shown(Math.abs(root.low) > Math.abs(root.high) ? root.low : root.high) }
    Label {
        id: valueLabel
        objectName: root.objectName + "_value"
        text: root.shown(root.value)
        color: Theme.textSecondary
        horizontalAlignment: Text.AlignRight
        Layout.preferredWidth: Math.ceil(widest.advanceWidth)
    }
    ToolButton {
        objectName: root.objectName + "_reset"
        icon.name: "rotate-ccw"
        enabled: root.value !== root.initial
        onClicked: { root.committed(root.initial); root.finished() }
        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
        ToolTip.text: qsTr("Put %1 back as it was").arg(root.label)
        Accessible.name: ToolTip.text
    }
}
