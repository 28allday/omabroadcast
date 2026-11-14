import QtQuick
import QtQuick.Controls
import OmaBroadcast 1.0

// The ⋮ at the end of a dock's header: where it goes, or closing it.
ToolButton {
    id: button
    signal requested(Item button)
    objectName: "dockMenuButton"
    icon.name: "ellipsis-vertical"
    onClicked: requested(button)
    ToolTip.visible: hovered
    ToolTip.delay: Theme.tooltipDelay
    ToolTip.text: qsTr("Dock")
    Accessible.name: qsTr("Dock options")
}
