import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Copies recordings into MP4 without encoding again: each new file lands
// beside its recording, which stays as it was.
Sheet {
    id: sheet
    helpTopic: "files"
    objectName: "remuxSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 560)
    modal: true
    title: qsTr("Remux recordings")
    standardButtons: Dialog.Close

    function stateText(job) {
        switch (job.state) {
        case "waiting": return qsTr("Waiting")
        case "running": return qsTr("%1%").arg(Math.round(job.progress * 100))
        case "done": return job.error !== "" ? qsTr("Made %1, but %2").arg(job.out, job.error) : qsTr("Made %1").arg(job.out)
        case "cancelled": return qsTr("Stopped")
        default: return job.error
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.s3
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            text: qsTr("Copies the picture and sound into an MP4 beside each recording, without encoding again, so it opens in editors and players that want MP4. Chapters and track names come along; the recording is left as it was.")
        }
        ListView {
            objectName: "remuxList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Theme.hRow * 8)
            visible: count > 0
            clip: true
            model: backend.remuxJobs
            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width
                height: Theme.hRow + Theme.s3
                spacing: Theme.s3
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s1
                    Label { text: modelData.name; color: Theme.textPrimary; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    ProgressBar {
                        Layout.fillWidth: true
                        visible: modelData.state === "running" || modelData.state === "waiting"
                        value: modelData.progress
                    }
                    Label {
                        visible: modelData.state !== "running" && modelData.state !== "waiting"
                        text: sheet.stateText(modelData)
                        color: modelData.state === "failed" ? Theme.warning : Theme.textMuted
                        font.pixelSize: Theme.fsCaption
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                }
                Label {
                    visible: modelData.state === "running"
                    text: sheet.stateText(modelData)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fsCaption
                }
                ToolButton {
                    visible: modelData.state === "running" || modelData.state === "waiting"
                    icon.name: "x"
                    onClicked: backend.cancelRemux(modelData.id)
                    ToolTip.text: qsTr("Stop this one; nothing is left behind")
                    Accessible.name: qsTr("Stop")
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "remuxAdd"
                text: qsTr("Add recordings…")
                icon.name: "plus"
                onClicked: backend.chooseRemuxFiles()
                ToolTip.text: qsTr("Choose recordings to copy into MP4")
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Clear finished")
                visible: backend.remuxJobs.some(j => j.state !== "running" && j.state !== "waiting")
                onClicked: backend.clearRemux()
                ToolTip.text: qsTr("Take finished ones off the list")
            }
        }
    }
}
