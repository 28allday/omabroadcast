import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// The transition a change of studio goes through and how long it takes;
// making, editing and removing transitions; studio mode.
ColumnLayout {
    id: controls
    spacing: Theme.s2
    signal propertiesRequested(string id)
    signal renameRequested(string id, string name)

    readonly property var current: backend.transitions.find(t => t.id === backend.currentTransition) ?? ({})
    // Cut has no time, and a stinger lasts as long as its video.
    readonly property bool timed: (backend.transitionKinds.find(k => k.id === (current.settings?.kind ?? "fade")) ?? {}).timed !== false

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.s1
        ComboBox {
            ToolTip.text: qsTr("How the program changes to another studio")
            objectName: "transitionChoice"
            Layout.fillWidth: true
            model: backend.transitions
            textRole: "name"
            valueRole: "id"
            currentIndex: backend.transitions.findIndex(t => t.id === backend.currentTransition)
            onActivated: backend.currentTransition = currentValue
            Accessible.name: qsTr("Transition")
        }
        ToolButton {
            objectName: "addTransitionButton"
            icon.name: "plus"
            onClicked: addMenu.popup()
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Make a transition")
            Accessible.name: ToolTip.text
            Menu {
                id: addMenu
                objectName: "addTransitionMenu"
                Instantiator {
                    model: backend.transitionKinds.filter(k => k.id !== "cut" && k.id !== "fade")
                    delegate: MenuItem {
                        required property var modelData
                        text: modelData.name
                        onTriggered: { const id = backend.addTransition(modelData.id); if (id !== "") controls.propertiesRequested(id) }
                    }
                    onObjectAdded: (index, object) => addMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => addMenu.removeItem(object)
                }
            }
        }
        ToolButton {
            objectName: "transitionMenuButton"
            icon.name: "ellipsis-vertical"
            onClicked: transitionMenu.popup()
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("More for this transition")
            Accessible.name: ToolTip.text
            Menu {
                id: transitionMenu
                objectName: "transitionMenu"
                MenuItem { text: qsTr("Properties…"); icon.name: "settings"; enabled: controls.current.builtIn === false
                           onTriggered: controls.propertiesRequested(backend.currentTransition) }
                MenuItem { text: qsTr("Rename…"); icon.name: "pencil"; enabled: controls.current.builtIn === false
                           onTriggered: controls.renameRequested(backend.currentTransition, controls.current.name) }
                MenuItem { objectName: "addQuickTransitionItem"; text: qsTr("Add as a quick transition")
                           onTriggered: backend.addQuickTransition(backend.currentTransition, controls.timed ? backend.transitionDuration : 0) }
                MenuSeparator {}
                MenuItem { text: qsTr("Remove"); icon.name: "trash-2"; enabled: controls.current.builtIn === false
                           onTriggered: backend.removeTransition(backend.currentTransition) }
            }
        }
    }
    RowLayout {
        visible: controls.timed
        Layout.fillWidth: true
        Label { text: qsTr("Duration"); color: Theme.textSecondary; Layout.fillWidth: true }
        SpinBox {
            ToolTip.text: qsTr("How long the transition takes, in milliseconds")
            objectName: "transitionDuration"
            from: 50; to: 20000; stepSize: 50
            editable: true
            value: backend.transitionDuration
            onValueModified: backend.transitionDuration = value
            Accessible.name: qsTr("Transition duration")
        }
        Label { text: qsTr("ms"); color: Theme.textMuted }
    }
    RowLayout {
        Layout.fillWidth: true
        CheckBox {
            objectName: "studioModeToggle"
            text: qsTr("Studio mode")
            checked: backend.studioMode
            onToggled: backend.studioMode = checked
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("Edit and preview a studio before taking it to the program")
        }
    }
}
