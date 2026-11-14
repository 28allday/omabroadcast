import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Settings → Hotkeys: a key for any action. Studio, source and filter keys
// belong to the collection; the rest to the profile. They work while the
// window is active; Apply global shortcuts asks the desktop for them
// everywhere.
ColumnLayout {
    id: page
    spacing: Theme.s2
    property string filter: ""
    property var all: []
    // Kept as it is when nothing in it changed: a new list would rebuild every row.
    function refresh() {
        const next = backend.actions()
        if (JSON.stringify(next) !== JSON.stringify(all)) all = next
    }
    Component.onCompleted: refresh()
    // A drag moves an item many times a second: the list is looked at once it settles.
    Timer { id: refreshLater; interval: 250; onTriggered: page.refresh() }
    readonly property var rows: all.filter(a => page.filter === ""
                                                       || a.name.toLowerCase().includes(page.filter.toLowerCase())
                                                       || a.group.toLowerCase().includes(page.filter.toLowerCase())
                                                       || (backend.hotkeys[a.id + "|" + a.argument] ?? "").toLowerCase().includes(page.filter.toLowerCase()))
    // Studios, sources and the rest change as the collection does.
    Connections { target: backend; function onScenesChanged() { refreshLater.restart() } function onItemsChanged() { refreshLater.restart() } }

    RowLayout {
        Layout.fillWidth: true
        TextField {
            objectName: "hotkeySearch"
            Layout.fillWidth: true
            placeholderText: qsTr("Find an action or a key")
            onTextChanged: page.filter = text
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Narrow the list by name, group or key")
        }
    }
    ListView {
        id: list
        objectName: "hotkeyList"
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: page.rows
        section.property: "group"
        section.delegate: SectionLabel { required property string section; text: section.toUpperCase(); width: ListView.view.width }
        delegate: RowLayout {
            id: row
            required property var modelData
            required property int index
            width: ListView.view.width - Theme.s3
            readonly property string key: backend.hotkeys[modelData.id + "|" + modelData.argument] ?? ""
            readonly property string clash: (backend.hotkeys, backend.hotkeyClash(modelData.id, modelData.argument, key))
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label { text: row.modelData.name; elide: Text.ElideRight; Layout.fillWidth: true }
                Label {
                    visible: row.clash !== ""
                    text: qsTr("Also: %1").arg(row.clash)
                    color: Theme.warning
                    font.pixelSize: Theme.fsLabel
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
            // One width for every key, so they make a tidy column on the right.
            KeyField {
                objectName: "hotkey-" + row.modelData.id + "|" + row.modelData.argument
                Layout.fillWidth: false
                Layout.preferredWidth: Theme.s6 * 6
                value: row.key
                onCommitted: (value) => backend.setHotkey(row.modelData.id, row.modelData.argument, value)
            }
        }
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textMuted
        font.pixelSize: Theme.fsLabel
        text: qsTr("Keys work while this window is active, outside dialogs and text fields. To use them while other apps have focus, apply them as global shortcuts; the desktop may ask you to confirm them. Apply again after changing keys. `omabroadcast ctl list` runs the same actions from a terminal or your compositor's bindings.")
    }
    RowLayout {
        Layout.fillWidth: true
        Label { text: qsTr("Keys work"); color: Theme.textSecondary }
        ComboBox {
            objectName: "hotkeyFocus"
            Layout.preferredWidth: Theme.s6 * 6
            model: [qsTr("Always"), qsTr("Only while OmaBroadcast has focus"), qsTr("Only while another app has focus")]
            readonly property var values: ["always", "focused", "unfocused"]
            currentIndex: Math.max(0, values.indexOf(backend.hotkeyFocus))
            onActivated: backend.hotkeyFocus = values[currentIndex]
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("When the action keys run; push-to-talk keys always work")
            Accessible.name: qsTr("When keys work")
        }
        Item { Layout.fillWidth: true }
    }
    RowLayout {
        Layout.fillWidth: true
        Button {
            objectName: "applyGlobalShortcuts"
            text: qsTr("Apply global shortcuts")
            enabled: !backend.audioHotkeysBusy
            onClicked: backend.applyGlobalHotkeys()
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("Ask the desktop to make these keys work everywhere")
        }
        Button {
            text: backend.audioHotkeysBusy ? qsTr("Cancel") : qsTr("Turn off global shortcuts")
            onClicked: backend.stopGlobalHotkeys()
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("Stop the keys working while other apps have focus")
        }
        Label { Layout.fillWidth: true; text: backend.audioHotkeyStatus; color: Theme.textMuted; elide: Text.ElideRight }
    }
}
