import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Files and folders the collection uses that are not where it says: point
// each at where it is now, or search a folder for all of them by name.
Sheet {
    id: sheet
    helpTopic: "files"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 560)
    modal: true
    title: qsTr("Find missing files")
    standardButtons: Dialog.Close

    contentItem: ColumnLayout {
        spacing: Theme.s3
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            text: backend.missingFiles.length === 0
                  ? qsTr("Every file this collection uses is where it should be.")
                  : qsTr("These have moved or are on another computer. Locate one and any that sat beside it follow.")
        }
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Theme.hRow * 8)
            clip: true
            model: backend.missingFiles
            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width
                height: Theme.hRow + Theme.s2
                spacing: Theme.s3
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label { text: modelData.name; color: Theme.textPrimary; elide: Text.ElideRight; Layout.fillWidth: true }
                    Label { text: modelData.path; color: Theme.textMuted; font.pixelSize: Theme.fsCaption
                            elide: Text.ElideMiddle; Layout.fillWidth: true }
                }
                Button {
                    ToolTip.text: qsTr("Find this file yourself")
                    text: qsTr("Locate…")
                    icon.name: modelData.folder ? "folder-open" : "file-text"
                    onClicked: backend.locateMissing(modelData.sourceId, modelData.index, modelData.key)
                }
            }
        }
        Button {
            ToolTip.text: qsTr("Look for every missing file in a folder and its subfolders")
            visible: backend.missingFiles.length > 0
            text: qsTr("Search a folder…")
            icon.name: "folder-open"
            onClicked: backend.searchForMissing()
        }
    }
}
