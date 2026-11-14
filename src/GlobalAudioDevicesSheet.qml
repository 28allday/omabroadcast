import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

Sheet {
    id: sheet
    helpTopic: "mixer"
    objectName: "globalAudioDevicesSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: qsTr("Global audio devices")
    width: Math.min(480, parent ? parent.width - Theme.s4 : 480)
    height: Math.min(implicitHeight, parent ? parent.height - Theme.s4 : implicitHeight)
    standardButtons: Dialog.Close
    contentItem: ScrollView {
        objectName: "globalAudioDevicesScroll"
        contentWidth: availableWidth
        implicitHeight: controls.implicitHeight
        clip: true
        ColumnLayout {
            id: controls
            width: parent.width
            spacing: Theme.s3
            Label {
                Layout.fillWidth: true
                text: qsTr("Add inputs or output captures that stay active across all studios and collections. Their controls appear in the mixer with your other sound.")
                wrapMode: Text.Wrap
                color: Theme.textSecondary
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Capturing the same device twice mixes it twice. Choose Off or mute any channel you do not need.")
                wrapMode: Text.Wrap
                color: Theme.textMuted
            }
            RowLayout {
                Layout.fillWidth: true
                ComboBox {
                    ToolTip.text: qsTr("An input (microphone, interface) or a capture of an output (what plays on speakers)")
                    id: kind
                    objectName: "globalAudioDeviceKind"
                    Layout.fillWidth: true
                    model: [qsTr("Audio input"), qsTr("Audio output capture")]
                    Accessible.name: qsTr("Device type")
                }
                Button {
                    ToolTip.text: qsTr("Add a channel of that kind")
                    objectName: "addGlobalAudioDevice"
                    text: qsTr("Add")
                    onClicked: backend.addGlobalAudioDevice(kind.currentIndex === 0 ? "audio-input" : "audio-output")
                }
            }
            Repeater {
                model: backend.globalAudioDevices.length
                delegate: ColumnLayout {
                    id: device
                    required property int index
                    readonly property var row: backend.globalAudioDevices[index] ?? ({})
                    Layout.fillWidth: true
                    spacing: Theme.s1
                    Label {
                        text: device.row.audioKind === "audio-output" ? qsTr("OUTPUT CAPTURE") : qsTr("INPUT")
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textMuted
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            ToolTip.text: qsTr("The channel's name in the mixer")
                            objectName: "globalAudioDeviceName-" + device.index
                            Layout.fillWidth: true
                            text: device.row.name ?? ""
                            selectByMouse: true
                            Accessible.name: qsTr("Channel name")
                            onEditingFinished: {
                                backend.setGlobalAudioDeviceSetting(device.row.id, "name", text)
                                text = Qt.binding(() => device.row.name ?? "")
                            }
                        }
                        ToolButton {
                            ToolTip.text: qsTr("Remove this channel")
                            objectName: "removeGlobalAudioDevice-" + device.index
                            text: qsTr("Remove")
                            onClicked: backend.removeGlobalAudioDevice(device.row.id)
                            Accessible.name: qsTr("Remove %1").arg(device.row.name ?? "")
                        }
                    }
                    AudioTargetPicker {
                        objectName: "globalAudioDeviceTarget-" + device.index
                        Layout.fillWidth: true
                        kind: device.row.audioKind ?? "audio-input"
                        choice: device.row.audioNode ?? ""
                        choiceName: device.row.audioLabel ?? ""
                        onChosen: (value) => backend.setGlobalAudioDeviceSetting(device.row.id, "audioNode", value)
                    }
                    Label {
                        visible: (device.row.error ?? "") !== ""
                        Layout.fillWidth: true
                        text: device.row.error ?? ""
                        wrapMode: Text.Wrap
                        color: Theme.textMuted
                    }
                }
            }
        }
    }
}
