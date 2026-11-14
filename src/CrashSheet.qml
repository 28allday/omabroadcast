import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// At a start after a run that did not quit cleanly: that run's log to read
// or keep, and a choice of starting in safe mode. Nothing starts until one
// is made.
Sheet {
    id: sheet
    helpTopic: "crashed"
    objectName: "crashSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 540)
    modal: true
    closePolicy: Popup.NoAutoClose
    title: qsTr("OmaBroadcast did not quit properly last time")

    signal logRequested()
    Component.onCompleted: if (backend.crashOffered) open()
    Connections {
        target: backend
        function onStartStateChanged() {
            if (backend.crashOffered && !sheet.visible) sheet.open()
            else if (!backend.crashOffered && sheet.visible) sheet.close()
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.s3
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            text: qsTr("It may have crashed, been stopped, or the computer lost power. Its log is kept here; nothing has been sent anywhere.")
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            text: backend.safeMode
                  ? qsTr("Starting in safe mode, as asked.")
                  : qsTr("If it went wrong while starting, start in safe mode: no screen captures are restored, nothing goes live or starts by itself, video filters leave out their files, and the picture is drawn without the graphics card.")
        }
        RowLayout {
            spacing: Theme.s2
            Button {
                objectName: "crashShowLog"
                ToolTip.text: qsTr("Read what the last run logged before it ended")
                text: qsTr("Show the log")
                icon.name: "list"
                onClicked: sheet.logRequested()
            }
            Button {
                objectName: "crashSaveLog"
                ToolTip.text: qsTr("Keep the last run's log in a file, for a bug report")
                text: qsTr("Save the log…")
                icon.name: "file-text"
                onClicked: backend.saveLog(true)
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.s2
            spacing: Theme.s2
            Item { Layout.fillWidth: true }
            Button {
                objectName: "crashStartNormally"
                ToolTip.text: qsTr("Start as usual, restoring captures and everything else")
                visible: !backend.safeMode
                text: qsTr("Start normally")
                onClicked: backend.startAfterCrash(false)
            }
            Button {
                objectName: "crashStartSafe"
                ToolTip.text: qsTr("Start without captures, filter files or the graphics card, to find what went wrong")
                highlighted: true
                text: backend.safeMode ? qsTr("Continue in safe mode") : qsTr("Start in safe mode")
                onClicked: backend.startAfterCrash(true)
            }
        }
    }
}
