import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// The hotkey sheet, on `?`.
Sheet {
    id: sheet
    objectName: "helpSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 480)
    modal: true
    title: qsTr("Keyboard shortcuts")
    standardButtons: Dialog.Close

    readonly property var keys: [
        ["Ctrl+R", qsTr("Start or stop recording")],
        ["Ctrl+Shift+R", qsTr("Save replay (while the replay buffer runs)")],
        ["Ctrl+Shift+D", qsTr("Add a display")],
        ["Ctrl+Shift+W", qsTr("Add a window")],
        ["Ctrl+G", qsTr("Group the selected sources")],
        ["Ctrl+C / Ctrl+V", qsTr("Copy sources / paste reference")],
        ["Ctrl+Shift+V", qsTr("Paste independent duplicates")],
        ["Ctrl+Alt+C / Ctrl+Alt+V", qsTr("Copy / paste placement")],
        ["Ctrl+wheel", qsTr("Zoom the preview")],
        ["Middle-drag", qsTr("Pan the preview")],
        ["Ctrl+0 / Ctrl+1", qsTr("Fit the preview / 100 %")],
        ["Ctrl+= / Ctrl+-", qsTr("Zoom in / out")],
        ["Ctrl+Shift+G", qsTr("Ungroup")],
        ["Ctrl+Z / Ctrl+Shift+Z", qsTr("Undo / redo")],
        ["Ctrl+,", qsTr("Settings")],
        ["F1", qsTr("The guide")],
        ["?", qsTr("This sheet")],
        ["Ctrl+Q", qsTr("Quit")]
    ]

    contentItem: GridLayout {
        columns: 2
        columnSpacing: Theme.s4
        rowSpacing: Theme.s1
        Repeater {
            model: sheet.keys
            delegate: Label {
                required property var modelData
                required property int index
                Layout.row: index
                Layout.column: 0
                text: modelData[0]
                font.family: Theme.monoFamily
                color: Theme.accent
            }
        }
        Repeater {
            model: sheet.keys
            delegate: Label {
                required property var modelData
                required property int index
                Layout.row: index
                Layout.column: 1
                Layout.fillWidth: true
                text: modelData[1]
                color: Theme.textSecondary
                wrapMode: Text.Wrap
            }
        }
    }
}
