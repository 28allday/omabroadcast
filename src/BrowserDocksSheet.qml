import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// View → Docks → Custom browser docks: pages that sit in the dock columns
// like the others (a chat, an alerts dashboard). Kept for every collection.
Sheet {
    id: sheet
    helpTopic: "layout"
    objectName: "browserDocksSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 620)
    modal: true
    title: qsTr("Custom browser docks")
    standardButtons: Dialog.Close
    // A new dock goes on the right, open.
    signal added(string id)

    contentItem: ColumnLayout {
        spacing: Theme.s3
        Label {
            Layout.fillWidth: true
            text: qsTr("Each dock shows a web page in the window, beside the others. Move, fold and close them from their ⋮ menu; closed ones come back from View → Docks.")
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }
        Repeater {
            model: backend.browserDocks
            delegate: RowLayout {
                id: row
                required property var modelData
                required property int index
                Layout.fillWidth: true
                spacing: Theme.s2
                TextField {
                    objectName: "browserDockName-" + row.index
                    Layout.preferredWidth: Theme.s5 * 5
                    text: row.modelData.name
                    ToolTip.text: qsTr("The dock's name, as its header shows it")
                    onEditingFinished: if (text.trim() !== "" && text !== row.modelData.name) backend.setBrowserDock(row.modelData.id, text, row.modelData.url)
                    Accessible.name: qsTr("Dock name")
                }
                TextField {
                    objectName: "browserDockUrl-" + row.index
                    Layout.fillWidth: true
                    text: row.modelData.url
                    ToolTip.text: qsTr("The page the dock shows")
                    inputMethodHints: Qt.ImhUrlCharactersOnly
                    onEditingFinished: if (text !== row.modelData.url) backend.setBrowserDock(row.modelData.id, row.modelData.name, text)
                    Accessible.name: qsTr("Dock address")
                }
                ToolButton {
                    objectName: "browserDockRemove-" + row.index
                    icon.name: "trash-2"
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Remove this dock")
                    Accessible.name: qsTr("Remove dock")
                    onClicked: backend.removeBrowserDock(row.modelData.id)
                }
            }
        }
        Label {
            visible: backend.browserDocks.length === 0
            text: qsTr("No browser docks yet.")
            color: Theme.textMuted
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: Theme.hairline; color: Theme.border }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s2
            TextField {
                id: newName
                objectName: "newBrowserDockName"
                Layout.preferredWidth: Theme.s5 * 5
                placeholderText: qsTr("Name")
                ToolTip.text: qsTr("What to call the new dock")
                Accessible.name: qsTr("New dock name")
            }
            TextField {
                id: newUrl
                objectName: "newBrowserDockUrl"
                Layout.fillWidth: true
                placeholderText: "https://"
                inputMethodHints: Qt.ImhUrlCharactersOnly
                ToolTip.text: qsTr("The page the new dock shows")
                Accessible.name: qsTr("New dock address")
                onAccepted: if (addButton.enabled) addButton.clicked()
            }
            Button {
                id: addButton
                objectName: "addBrowserDock"
                text: qsTr("Add")
                icon.name: "plus"
                enabled: newUrl.text.trim() !== ""
                ToolTip.text: qsTr("Add the dock and open it on the right")
                onClicked: {
                    const id = backend.addBrowserDock(newName.text, newUrl.text)
                    newName.text = ""
                    newUrl.text = ""
                    sheet.added(id)
                }
            }
        }
    }
}
