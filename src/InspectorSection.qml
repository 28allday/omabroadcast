import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import QtQuick.Layouts
import OmaBroadcast 1.0

// A group of inspector controls under a caption that folds it away. Whether
// it is open is remembered app-wide under `key`.
ColumnLayout {
    id: section
    property string title
    property string key
    property bool startOpen: true
    property bool collapsible: true
    property bool open: true
    // Beside the caption, on its right (an Edit… button).
    property alias trailing: trailingRow.data
    default property alias content: body.data
    Layout.fillWidth: true
    spacing: Theme.s2

    Component.onCompleted: open = !collapsible || key === "" || backend.panelOpen(key, startOpen)
    function toggle() {
        open = !open
        if (key !== "") backend.setPanelOpen(key, open)
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: Theme.s2
        spacing: Theme.s1
        T.AbstractButton {
            id: header
            objectName: "sectionHeader"
            Layout.fillWidth: true
            implicitHeight: Theme.hControl
            enabled: section.collapsible
            hoverEnabled: true
            focusPolicy: Qt.TabFocus
            onClicked: section.toggle()
            Accessible.role: Accessible.Button
            Accessible.name: section.title
            Accessible.description: section.open ? qsTr("Expanded") : qsTr("Collapsed")
            T.ToolTip.visible: hovered && section.collapsible
            T.ToolTip.delay: Theme.tooltipDelay
            T.ToolTip.text: section.open ? qsTr("Fold %1 away").arg(section.title.toLowerCase()) : qsTr("Show %1").arg(section.title.toLowerCase())
            contentItem: RowLayout {
                spacing: Theme.s1
                Icon {
                    visible: section.collapsible
                    name: section.open ? "chevron-down" : "chevron-right"
                    size: Theme.szIcon - 2
                    color: header.hovered ? Theme.textPrimary : Theme.textMuted
                }
                Label {
                    Layout.fillWidth: true
                    text: section.title.toUpperCase()
                    color: header.hovered ? Theme.textSecondary : Theme.textMuted
                    font.pixelSize: Theme.fsCaption
                    font.weight: Theme.wHeading
                    font.letterSpacing: Theme.capsTracking
                }
            }
            background: Rectangle {
                radius: Theme.rControl
                color: header.visualFocus ? Theme.controlHover : "transparent"
            }
        }
        RowLayout { id: trailingRow; spacing: Theme.s1 }
    }

    ColumnLayout {
        id: body
        visible: section.open
        Layout.fillWidth: true
        spacing: Theme.s2
    }
}
