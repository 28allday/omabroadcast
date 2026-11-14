import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Asks for a name: scenes and sources are renamed through it.
Sheet {
    id: sheet
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 420)
    modal: true
    standardButtons: Dialog.Cancel | Dialog.Ok
    property var apply: null

    function ask(heading, current, run) {
        title = heading
        field.text = current
        apply = run
        open()
        field.selectAll()
        field.forceActiveFocus()
    }
    onAccepted: { const run = apply; apply = null; if (run && field.text.trim() !== "") run(field.text.trim()) }
    onRejected: apply = null

    contentItem: TextField {
        ToolTip.text: qsTr("The new name")
        id: field
        onAccepted: sheet.accept()
    }
}
