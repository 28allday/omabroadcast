import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

Sheet {
    id: sheet
    helpTopic: "profiles"
    objectName: "profilesSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 640)
    height: Math.min(parent.height - Theme.s5 * 2, 480)
    modal: true
    title: qsTr("Profiles")
    standardButtons: Dialog.Close
    // Switching restarts the session, which would end every output.
    // One width for its buttons, so the two rows line up.
    readonly property int buttonWidth: Theme.s6 * 2 + Theme.s4
    readonly property bool canChange: !backend.recording && !backend.recordingStopping && !backend.streaming
                                      && !backend.replayActive && !backend.virtualCamera

    contentItem: ColumnLayout {
        spacing: Theme.s3
        Label {
            Layout.fillWidth: true
            text: qsTr("Save video, recording and audio settings for different setups. Switching profiles keeps the current studio collection.")
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }
        Label {
            visible: !sheet.canChange
            Layout.fillWidth: true
            text: qsTr("Stop recording, streaming, the replay buffer and the virtual camera before changing profiles.")
            wrapMode: Text.Wrap
            color: Theme.textSecondary
        }
        ListView {
            id: list
            objectName: "profilesList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: backend.profiles
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                required property var modelData
                width: list.width
                text: modelData.name + (modelData.current ? qsTr("  ·  Current") : "")
                highlighted: modelData.current
                enabled: sheet.canChange
                onClicked: backend.switchProfile(modelData.id)
                Accessible.name: text
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Button { objectName: "newProfileButton"; Layout.preferredWidth: Math.max(implicitWidth, sheet.buttonWidth); text: qsTr("New…"); enabled: sheet.canChange
                ToolTip.text: qsTr("A new profile with default settings")
                onClicked: nameSheet.ask(qsTr("New profile"),qsTr("Profile"),(name) => backend.newProfile(name)) }
            Button { objectName: "duplicateProfileButton"; Layout.preferredWidth: Math.max(implicitWidth, sheet.buttonWidth); text: qsTr("Duplicate"); enabled: sheet.canChange; onClicked: backend.duplicateProfile(); ToolTip.text: qsTr("A copy of the current profile") }
            Button { objectName: "renameProfileButton"; Layout.preferredWidth: Math.max(implicitWidth, sheet.buttonWidth); text: qsTr("Rename…"); enabled: sheet.canChange
                ToolTip.text: qsTr("Give the current profile a new name")
                onClicked: nameSheet.ask(qsTr("Rename profile"),backend.profileName,(name) => backend.renameProfile(name)) }
            Button { objectName: "removeProfileButton"; Layout.preferredWidth: Math.max(implicitWidth, sheet.buttonWidth); text: qsTr("Remove…"); enabled: sheet.canChange && backend.profiles.length > 1
                ToolTip.text: qsTr("Move the current profile to the bin")
                onClicked: { removeSheet.target = backend.profileId; removeSheet.label = backend.profileName; removeSheet.open() } }
        }
        RowLayout {
            Button { objectName: "importProfileButton"; Layout.preferredWidth: Math.max(implicitWidth, sheet.buttonWidth); text: qsTr("Import…"); enabled: sheet.canChange; onClicked: backend.importProfile(); ToolTip.text: qsTr("Add a profile from an exported file") }
            Button { objectName: "exportProfileButton"; Layout.preferredWidth: Math.max(implicitWidth, sheet.buttonWidth); text: qsTr("Export…"); onClicked: backend.exportProfile(); ToolTip.text: qsTr("Save the current profile to a file for another computer; capture permissions, stream keys and passwords are left out") }
        }
    }
    NameSheet { id: nameSheet; objectName: "profileNameSheet" }
    Sheet {
        id: removeSheet
        objectName: "removeProfileSheet"
        property string target: ""
        property string label: ""
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s5 * 2,420)
        modal: true
        title: qsTr("Remove this profile?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        contentItem: Label { text: qsTr("Move “%1” to the bin? Your collections and recordings stay where they are.").arg(removeSheet.label)
            color: Theme.textSecondary; wrapMode: Text.Wrap }
        onAccepted: backend.removeProfile(target)
    }
}
