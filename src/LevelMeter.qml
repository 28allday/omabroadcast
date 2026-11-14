import QtQuick
import QtQuick.Controls
import OmaBroadcast 1.0

// Fixed dB regions: the amber/red thresholds never move with the fill level.
Item {
    id: meter
    property real level: 0
    property bool muted: false
    property bool vertical: false
    property bool clipped: false
    property color lowColour: Theme.meterLow
    property color midColour: Theme.meterMid
    property color highColour: Theme.meterHigh
    implicitHeight: vertical ? 160 : Theme.hMeter
    implicitWidth: vertical ? Theme.hMeter : 160
    Rectangle { anchors.fill: parent; radius: Theme.rChip; color: Theme.meterTrack }
    Item {
        anchors.fill: parent
        opacity: meter.muted ? Theme.disabledOpacity : 1
        Repeater {
            model: [{start: 0, end: 0.666666667, tint: meter.lowColour},
                    {start: 0.666666667, end: 0.9, tint: meter.midColour},
                    {start: 0.9, end: 1, tint: meter.highColour}]
            delegate: Rectangle {
                required property var modelData
                readonly property real amount: Math.max(0, Math.min(modelData.end, meter.level) - modelData.start)
                x: meter.vertical ? 0 : meter.width * modelData.start
                y: meter.vertical ? meter.height * (1 - modelData.start - amount) : 0
                width: meter.vertical ? meter.width : meter.width * amount
                height: meter.vertical ? meter.height * amount : meter.height
                color: modelData.tint
            }
        }
        Rectangle {
            visible: meter.clipped
            anchors.top: parent.top
            anchors.right: parent.right
            width: meter.vertical ? parent.width : 3
            height: meter.vertical ? 3 : parent.height
            color: meter.highColour
        }
    }
    Accessible.name: qsTr("Audio level")
    Accessible.description: meter.level <= 0 ? qsTr("Silence") : qsTr("%1 decibels").arg((meter.level * 60 - 60).toFixed(1))
}
