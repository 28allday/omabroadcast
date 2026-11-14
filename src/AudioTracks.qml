import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

RowLayout {
    id: root
    property int mask: 1
    signal chosen(int mask)
    spacing: Theme.s1
    Repeater {
        model: 6
        ToolButton {
            required property int index
            objectName: "audioTrack" + (index + 1)
            Layout.fillWidth: true
            text: String(index + 1)
            checkable: true
            checked: (root.mask & (1 << index)) !== 0
            onClicked: root.chosen(checked ? root.mask | (1 << index) : root.mask & ~(1 << index))
            Accessible.name: qsTr("Audio track %1").arg(index + 1)
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("Audio track %1").arg(index + 1)
        }
    }
}
