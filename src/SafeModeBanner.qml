import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Across the window while in safe mode: what is held back, and the way out.
Rectangle {
    id: banner
    objectName: "safeModeBanner"
    signal restartRequested()
    visible: backend.safeMode
    implicitHeight: visible ? row.implicitHeight + Theme.s2 * 2 : 0
    color: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.14)
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.warning }
    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: Theme.s4
        anchors.rightMargin: Theme.s3
        spacing: Theme.s3
        Icon { name: "circle-alert"; color: Theme.warning }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            textFormat: Text.StyledText
            text: qsTr("<b>Safe mode.</b> Screen captures were not restored, nothing starts by itself, video filters leave out their files, and the picture is drawn without the graphics card.")
        }
        Button {
            objectName: "restartNormally"
            ToolTip.text: qsTr("Quit and start again as usual")
            text: qsTr("Restart normally")
            onClicked: banner.restartRequested()
        }
    }
}
