import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Tools → Remote control: stream controllers, Stream Deck plugins and
// phone apps that speak the version 5 WebSocket remote-control protocol
// connect here to switch studios, start recording and the rest.
Sheet {
    id: sheet
    helpTopic: "tools"
    objectName: "remoteControlSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 520)
    modal: true
    title: qsTr("Remote control")
    standardButtons: Dialog.Close
    readonly property var remote: backend.remoteControl
    property bool showPassword: false

    contentItem: ColumnLayout {
        spacing: Theme.s2
        CheckBox {
            objectName: "remoteEnabled"
            text: qsTr("Allow remote control")
            checked: sheet.remote.enabled
            onToggled: backend.setRemoteOption("enabled", checked)
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("Let controllers and apps that speak the version 5 WebSocket remote-control protocol connect")
        }
        RowLayout {
            spacing: Theme.s2
            Label { text: qsTr("Port"); color: Theme.textSecondary }
            SpinBox {
                objectName: "remotePort"
                from: 1024; to: 65535; editable: true
                value: sheet.remote.port
                textFromValue: (value) => String(value)
                valueFromText: (text) => parseInt(text)
                onValueModified: backend.setRemoteOption("port", value)
                Accessible.name: qsTr("Remote control port")
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                ToolTip.text: qsTr("The port controllers connect to; 4455 is what they expect unless told otherwise")
            }
        }
        CheckBox {
            objectName: "remoteNetwork"
            text: qsTr("Other devices on the network can connect")
            checked: sheet.remote.network
            onToggled: backend.setRemoteOption("network", checked)
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("Off: only apps on this computer can connect")
        }
        CheckBox {
            objectName: "remoteAuth"
            text: qsTr("Require a password")
            checked: sheet.remote.auth
            onToggled: backend.setRemoteOption("auth", checked)
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("Controllers must give this password before they can do anything")
        }
        RowLayout {
            Layout.fillWidth: true
            enabled: sheet.remote.auth
            spacing: Theme.s2
            TextField {
                id: password
                objectName: "remotePassword"
                Layout.fillWidth: true
                text: sheet.remote.password
                echoMode: sheet.showPassword ? TextInput.Normal : TextInput.Password
                placeholderText: qsTr("Password")
                Accessible.name: qsTr("Remote control password")
                // Controllers always need one: cleared, it goes back to the one in force.
                onEditingFinished: {
                    if (text !== sheet.remote.password && text.trim() !== "") backend.setRemoteOption("password", text)
                    text = Qt.binding(() => sheet.remote.password)
                }
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                ToolTip.text: qsTr("The password controllers sign in with; type your own or make a new one")
            }
            ToolButton {
                text: sheet.showPassword ? qsTr("Hide") : qsTr("Show")
                onClicked: sheet.showPassword = !sheet.showPassword
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Show or hide the password")
            }
            ToolButton {
                text: qsTr("Copy")
                onClicked: backend.copyText(sheet.remote.password)
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Copy the password")
            }
            ToolButton {
                objectName: "remoteNewPassword"
                text: qsTr("New")
                // With controllers connected it signs them out: asked first.
                onClicked: (sheet.remote.clients ?? []).length > 0 ? newPasswordSheet.open()
                                                                  : backend.setRemoteOption("password", backend.newRemotePassword())
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                ToolTip.text: qsTr("Make a new password; connected controllers must sign in again")
            }
        }
        Label {
            objectName: "remoteStatus"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: sheet.remote.error ? Theme.danger : Theme.textSecondary
            text: sheet.remote.error ? sheet.remote.error
                : !sheet.remote.listening ? qsTr("Off.")
                : sheet.remote.clients.length === 0 ? qsTr("Waiting for a controller.")
                : qsTr("%n controller(s) connected: %1", "", sheet.remote.clients.length).arg(sheet.remote.clients.map(c => c.address).join(", "))
        }
        Label {
            objectName: "remoteAddresses"
            Layout.fillWidth: true
            visible: sheet.remote.listening
            wrapMode: Text.Wrap
            color: Theme.textMuted
            font.pixelSize: Theme.fsLabel
            text: {
                const hosts = sheet.remote.network && sheet.remote.addresses.length ? sheet.remote.addresses : ["localhost"]
                return qsTr("Connect to %1").arg(hosts.map(h => "ws://" + h + ":" + sheet.remote.port).join(qsTr(" or ")))
            }
        }
    }

    Sheet {
        id: newPasswordSheet
        objectName: "remoteNewPasswordSheet"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s5 * 2, 420)
        modal: true
        title: qsTr("Make a new password?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        contentItem: Label {
            text: qsTr("The controllers connected now are signed out and need the new password.")
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }
        onAccepted: backend.setRemoteOption("password", backend.newRemotePassword())
    }
}
