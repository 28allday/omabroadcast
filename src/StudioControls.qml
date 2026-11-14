import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Between the preview and the program in studio mode: take the previewed
// studio to the program, through the chosen transition, a quick one, or the
// T-bar by hand.
ColumnLayout {
    id: controls
    spacing: Theme.s2
    readonly property bool canTake: backend.currentScene !== backend.programScene || backend.takeCopy || backend.tBarActive

    Item { Layout.fillHeight: true }
    Button {
        objectName: "takeButton"
        Layout.fillWidth: true
        text: qsTr("Take")
        icon.name: "chevron-right"
        highlighted: true
        enabled: controls.canTake
        onClicked: backend.take()
        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
        ToolTip.text: qsTr("Put the previewed studio on the program")
    }
    Repeater {
        objectName: "quickTransitions"
        model: backend.quickTransitions
        delegate: Button {
            required property var modelData
            required property int index
            objectName: "quickTransition-" + index
            Layout.fillWidth: true
            flat: true
            enabled: controls.canTake
            text: modelData.durationMs > 0 ? qsTr("%1 %2 ms").arg(modelData.name).arg(modelData.durationMs) : modelData.name
            onClicked: backend.quickTake(index)
            TapHandler { acceptedButtons: Qt.RightButton; onTapped: { quickMenu.index = index; quickMenu.popup() } }
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("Take with this transition (right-click to remove)")
        }
    }
    Menu {
        id: quickMenu
        property int index: -1
        MenuItem { text: qsTr("Remove quick transition"); icon.name: "trash-2"; onTriggered: backend.removeQuickTransition(quickMenu.index) }
    }
    // The T-bar: drag it to mix by hand; let go near either end and it goes there.
    Slider {
        id: tBar
        objectName: "tBar"
        Layout.alignment: Qt.AlignHCenter
        Layout.preferredHeight: Theme.s6 * 4
        orientation: Qt.Vertical
        from: 1; to: 0
        enabled: controls.canTake
        value: 0
        // Shows where a transition has got to while it is not held.
        Connections {
            target: backend
            function onTransitionStateChanged() { if (!tBar.pressed) tBar.value = backend.transitioning ? backend.transitionProgress : 0 }
        }
        onMoved: backend.setTBar(value)
        onPressedChanged: if (!pressed) backend.releaseTBar()
        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
        ToolTip.text: qsTr("Drag to mix to the previewed studio by hand")
        Accessible.name: qsTr("T-bar")
    }
    CheckBox {
        objectName: "swapAfterTake"
        text: qsTr("Swap")
        checked: backend.swapAfterTake
        onToggled: backend.swapAfterTake = checked
        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
        ToolTip.text: qsTr("After a take, preview what was on the program")
    }
    CheckBox {
        objectName: "takeCopy"
        text: qsTr("Copy")
        checked: backend.takeCopy
        onToggled: backend.takeCopy = checked
        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
        ToolTip.text: qsTr("Take a copy of the layout, so edits stay off the program until the next take")
    }
    CheckBox {
        objectName: "takeCopySources"
        text: qsTr("Copy sources")
        enabled: backend.takeCopy
        checked: backend.takeCopySources
        onToggled: backend.takeCopySources = checked
        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
        ToolTip.text: qsTr("The copy keeps its own colours, images, text and slideshows as they were, so changing those stays off the program too")
    }
    Item { Layout.fillHeight: true }
}
