import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import OmaBroadcast 1.0

// The controls that change what leaves the machine, so they never look like
// any other: an outlined well with a red dot, filled red while running.
// Record by default; Go live gives it its own words.
T.Button {
    id: control
    property bool recording: false
    property bool stopping: false
    property string idleText: qsTr("Record")
    property string activeText: qsTr("Stop")
    property string stoppingText: qsTr("Finishing…")
    property string startTip: qsTr("Start recording")
    property string stopTip: qsTr("Stop recording")
    property string stoppingTip: qsTr("Finishing recording")
    property string shortcutText: "Ctrl+R"
    enabled: !stopping

    text: stopping ? stoppingText : recording ? activeText : idleText
    implicitWidth: content.implicitWidth + Theme.s3 * 2
    implicitHeight: Theme.hControl + Theme.s1
    hoverEnabled: true
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsControl
    font.weight: Theme.wHeading
    Accessible.name: stopping ? stoppingTip : recording ? stopTip : startTip
    ToolTip.visible: hovered
    ToolTip.delay: Theme.tooltipDelay
    ToolTip.text: (recording ? stopTip : startTip) + (shortcutText ? "  " + shortcutText : "")

    readonly property color ink: Theme.textPrimary

    contentItem: Item {
        Row {
            id: content
            anchors.centerIn: parent
            spacing: Theme.s2
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.szStatusDot + 2
                height: width
                radius: control.recording ? 2 : width / 2
                color: control.recording ? Theme.textPrimary : Theme.recording
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                textFormat: Text.PlainText
                font: control.font
                color: control.ink
            }
        }
    }

    background: Rectangle {
        radius: Theme.rControl
        color: control.recording ? (control.down ? Theme.pressedOn(Theme.recording) : control.hovered ? Theme.hovered(Theme.recording) : Theme.recording)
             : control.down ? Theme.pressedOn(Theme.controlBg) : control.hovered ? Theme.controlHover : Theme.controlBg
        border.width: control.visualFocus ? Theme.focusRing + 1 : Theme.hairline
        border.color: control.visualFocus ? Theme.accent : control.recording ? Theme.recording : Theme.borderStrong
        Behavior on color { ColorAnimation { duration: Theme.dFast } }
    }
}
