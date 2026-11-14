import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Tools → Automatic studio switcher: the program follows the window that
// has the keyboard. Rules are tried top to bottom; a pattern is a regular
// expression, matched without regard to case.
Sheet {
    id: sheet
    helpTopic: "tools"
    objectName: "switcherSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 640)
    modal: true
    title: qsTr("Automatic studio switcher")
    standardButtons: Dialog.Close
    readonly property var studios: backend.scenes
    function setRule(index, key, value) {
        const rules = backend.switcherRules.slice()
        rules[index] = Object.assign({}, rules[index], { [key]: value })
        backend.switcherRules = rules
    }

    contentItem: ColumnLayout {
        spacing: Theme.s2
        Label {
            visible: !backend.switcherAvailable
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.warning
            text: qsTr("This needs Hyprland, which says which window has the keyboard.")
        }
        CheckBox {
            objectName: "switcherEnabled"
            text: qsTr("Switch studios by the focused window")
            checked: backend.switcherEnabled
            onToggled: backend.switcherEnabled = checked
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("Change the program's studio when another window gets the keyboard")
        }
        RowLayout {
            Layout.fillWidth: true
            Label { text: qsTr("Focused now:"); color: Theme.textSecondary }
            Label { objectName: "focusedWindow"; Layout.fillWidth: true; elide: Text.ElideRight
                    text: backend.focusedWindow !== "" ? backend.focusedWindow : qsTr("(not known yet)") }
            Button {
                objectName: "addSwitcherRule"
                text: qsTr("Add a rule")
                onClicked: backend.switcherRules = backend.switcherRules.concat([{ field: "class", pattern: "", studio: sheet.studios.length ? sheet.studios[0].id : "" }])
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("A new rule, at the end")
            }
        }
        ListView {
            objectName: "switcherRules"
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.hRow * 6
            clip: true
            model: backend.switcherRules
            delegate: RowLayout {
                id: ruleRow
                required property var modelData
                required property int index
                width: ListView.view.width
                ComboBox {
                    ToolTip.text: qsTr("Match the window's class (its app) or its title")
                    model: [qsTr("Class"), qsTr("Title")]
                    currentIndex: ruleRow.modelData.field === "title" ? 1 : 0
                    onActivated: sheet.setRule(ruleRow.index, "field", currentIndex === 1 ? "title" : "class")
                    Accessible.name: qsTr("Match")
                }
                TextField {
                    objectName: "switcherPattern-" + ruleRow.index
                    Layout.fillWidth: true
                    text: ruleRow.modelData.pattern
                    placeholderText: qsTr("e.g. firefox|chromium")
                    onEditingFinished: if (text !== ruleRow.modelData.pattern) sheet.setRule(ruleRow.index, "pattern", text)
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: qsTr("A regular expression; it may match any part")
                }
                ComboBox {
                    ToolTip.text: qsTr("The studio to show when this matches")
                    Layout.preferredWidth: Theme.s6 * 4
                    model: sheet.studios
                    textRole: "name"
                    valueRole: "id"
                    currentIndex: sheet.studios.findIndex(s => s.id === ruleRow.modelData.studio)
                    onActivated: sheet.setRule(ruleRow.index, "studio", currentValue)
                    Accessible.name: qsTr("Studio")
                }
                ToolButton {
                    icon.name: "arrow-up"; enabled: ruleRow.index > 0
                    onClicked: { const r = backend.switcherRules.slice(); r.splice(ruleRow.index - 1, 0, r.splice(ruleRow.index, 1)[0]); backend.switcherRules = r }
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Try this rule earlier"); Accessible.name: ToolTip.text
                }
                ToolButton {
                    icon.name: "trash-2"
                    onClicked: { const r = backend.switcherRules.slice(); r.splice(ruleRow.index, 1); backend.switcherRules = r }
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Remove this rule"); Accessible.name: ToolTip.text
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Label { text: qsTr("When nothing matches"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox {
                ToolTip.text: qsTr("What the program does when no rule matches the focused window")
                objectName: "switcherOtherwise"
                Layout.preferredWidth: Theme.s6 * 5
                readonly property var choices: [{ id: "", name: qsTr("Stay where it is") }].concat(sheet.studios)
                model: choices
                textRole: "name"
                valueRole: "id"
                currentIndex: Math.max(0, choices.findIndex(s => s.id === backend.switcherOtherwise))
                onActivated: backend.switcherOtherwise = currentValue
                Accessible.name: qsTr("When nothing matches")
            }
        }
    }
}
