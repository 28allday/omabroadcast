import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0
import "numbers.js" as Numbers

// Tools → Scripts: JavaScript files that drive the app (see the manual for
// what they can do), each switched on or off, reloaded or removed, and
// what they write.
Sheet {
    id: sheet
    helpTopic: "tools"
    objectName: "scriptsSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 640)
    height: Math.min(parent.height - Theme.s5 * 2, 560)
    modal: true
    title: qsTr("Scripts")
    standardButtons: Dialog.Close
    // The script whose settings show below the list.
    property string selected: ""
    readonly property var form: (backend.scripts, selected !== "" ? backend.scriptForm(selected) : [])

    contentItem: ColumnLayout {
        spacing: Theme.s2
        RowLayout {
            Layout.fillWidth: true
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textMuted; font.pixelSize: Theme.fsLabel
                    text: qsTr("Scripts run the same actions as hotkeys and `omabroadcast ctl`, follow what happens, and keep time. They cannot read files or reach the network.") }
            Button {
                objectName: "addScript"
                text: qsTr("Add a script…")
                icon.name: "plus"
                onClicked: backend.addScript()
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Choose a .js file")
            }
        }
        ListView {
            objectName: "scriptList"
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.hRow * 4
            clip: true
            model: backend.scripts
            delegate: RowLayout {
                id: row
                required property var modelData
                width: ListView.view.width
                CheckBox {
                    text: row.modelData.name
                    checked: row.modelData.enabled
                    onToggled: backend.setScriptEnabled(row.modelData.path, checked)
                    Layout.fillWidth: true
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: row.modelData.path
                }
                Label { text: row.modelData.running ? qsTr("running") : qsTr("off"); color: Theme.textMuted }
                ToolButton {
                    objectName: "scriptSettings-" + row.modelData.name
                    icon.name: "settings"; checkable: true; checked: sheet.selected === row.modelData.path
                    onClicked: sheet.selected = checked ? row.modelData.path : ""
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Its settings"); Accessible.name: ToolTip.text
                }
                ToolButton {
                    icon.name: "rotate-ccw"; enabled: row.modelData.running
                    onClicked: backend.reloadScript(row.modelData.path)
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Load it again, after changing the file"); Accessible.name: ToolTip.text
                }
                ToolButton {
                    icon.name: "trash-2"
                    onClicked: backend.removeScript(row.modelData.path)
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Remove it (the file stays)"); Accessible.name: ToolTip.text
                }
            }
        }
        SectionLabel { visible: sheet.selected !== ""; text: qsTr("SETTINGS") }
        Label { visible: sheet.selected !== "" && sheet.form.length === 0; color: Theme.textMuted; text: qsTr("This script has no settings.") }
        Repeater {
            model: sheet.form
            delegate: RowLayout {
                id: field
                required property var modelData
                Layout.fillWidth: true
                Label { text: field.modelData.label; color: Theme.textSecondary; Layout.fillWidth: true }
                TextField {
                    objectName: "scriptField-" + field.modelData.key
                    visible: field.modelData.type === "text" || field.modelData.type === "number"
                    Layout.preferredWidth: Theme.s6 * 5
                    text: String(field.modelData.value ?? "")
                    validator: field.modelData.type === "number" ? numbers : null
                    onEditingFinished: {
                        if (field.modelData.type !== "number") { backend.setScriptSetting(sheet.selected, field.modelData.key, text); return }
                        const v = Numbers.read(text, Qt.locale().decimalPoint)
                        if (!isNaN(v)) backend.setScriptSetting(sheet.selected, field.modelData.key, v)
                        text = Qt.binding(() => String(field.modelData.value ?? ""))
                    }
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: field.modelData.label
                    RegularExpressionValidator { id: numbers; regularExpression: Numbers.typing }
                }
                CheckBox {
                    visible: field.modelData.type === "bool"
                    checked: field.modelData.value === true
                    onToggled: backend.setScriptSetting(sheet.selected, field.modelData.key, checked)
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: field.modelData.label
                }
                ComboBox {
                    ToolTip.text: field.modelData.label
                    visible: field.modelData.type === "studio"
                    Layout.preferredWidth: Theme.s6 * 5
                    model: backend.scenes
                    textRole: "name"
                    valueRole: "name"
                    currentIndex: backend.scenes.findIndex(s => s.name === field.modelData.value || s.id === field.modelData.value)
                    onActivated: backend.setScriptSetting(sheet.selected, field.modelData.key, currentValue)
                    Accessible.name: field.modelData.label
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            SectionLabel { text: qsTr("LOG"); Layout.fillWidth: true }
            ToolButton {
                icon.name: "trash-2"
                onClicked: backend.clearScriptLog()
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Clear the log"); Accessible.name: ToolTip.text
            }
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            TextArea {
                objectName: "scriptLog"
                readOnly: true
                wrapMode: TextEdit.Wrap
                font.family: Theme.monoFamily
                text: backend.scriptLog.join("\n")
                ToolTip.text: qsTr("What the scripts wrote, and their errors")
            }
        }
    }
}
