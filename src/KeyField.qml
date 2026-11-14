import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// A key for a hotkey: click, then press it. Backspace or Delete clears it;
// Escape leaves it as it was.
RowLayout {
    id: root
    property string value: ""
    property bool listening: false
    signal committed(string value)
    spacing: Theme.s1

    Button {
        id: capture
        objectName: "keyCapture"
        Layout.fillWidth: true
        checkable: true
        checked: root.listening
        text: root.listening ? qsTr("Press a key…") : root.value !== "" ? root.value : qsTr("None")
        font.family: root.value !== "" && !root.listening ? Theme.monoFamily : Theme.fontFamily
        onClicked: { root.listening = true; forceActiveFocus() }
        onActiveFocusChanged: if (!activeFocus) root.listening = false
        // While it listens every key is its own: Escape must not close the
        // sheet around it, nor a key being set run what it is set to.
        Keys.onShortcutOverride: (event) => event.accepted = root.listening
        Keys.onPressed: (event) => {
            if (!root.listening) return
            event.accepted = true
            if (event.key === Qt.Key_Escape) { root.listening = false; return }
            if (event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete) { root.listening = false; root.committed(""); return }
            const text = backend.keyText(event.key, event.modifiers)
            if (text === "") return   // a modifier on its own: wait for the key
            root.listening = false
            root.committed(text)
        }
        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
        ToolTip.text: qsTr("Click, then press the key (with Ctrl, Alt, Shift or Super if you like); Backspace clears it")
        Accessible.name: qsTr("Key")
    }
    // Always takes its room, so a column of keys lines up whether set or not.
    ToolButton {
        enabled: root.value !== ""
        opacity: enabled ? 1 : 0
        icon.name: "x"
        onClicked: root.committed("")
        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Clear the key")
        Accessible.name: ToolTip.text
    }
}
