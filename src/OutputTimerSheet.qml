import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Tools → Output timer: stop a recording or a stream after a while. Each
// counts from when it starts (or from now, if it already runs); a paused
// recording's pause does not count.
Sheet {
    id: sheet
    helpTopic: "tools"
    objectName: "outputTimerSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 460)
    modal: true
    title: qsTr("Output timer")
    standardButtons: Dialog.Close
    function left(seconds) {
        return seconds < 0 ? "" : qsTr("%1:%2 left").arg(Math.floor(seconds / 60)).arg(String(seconds % 60).padStart(2, "0"))
    }

    contentItem: GridLayout {
        columns: 3
        columnSpacing: Theme.s3
        CheckBox {
            id: recordOn
            objectName: "recordTimerOn"
            text: qsTr("Stop recording after")
            checked: backend.recordTimerSeconds > 0
            onToggled: backend.recordTimerSeconds = checked ? recordMinutes.value * 60 : 0
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Stop each recording once it has run this long")
        }
        SpinBox {
            id: recordMinutes
            objectName: "recordTimerMinutes"
            from: 1; to: 10080; editable: true
            value: backend.recordTimerSeconds > 0 ? Math.max(1, Math.round(backend.recordTimerSeconds / 60)) : 60
            onValueModified: if (recordOn.checked) backend.recordTimerSeconds = value * 60
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Minutes")
            Accessible.name: qsTr("Recording minutes")
        }
        Label { text: sheet.left(backend.recordTimerLeft) || qsTr("minutes"); color: Theme.textMuted }
        CheckBox {
            id: streamOn
            objectName: "streamTimerOn"
            text: qsTr("End the stream after")
            checked: backend.streamTimerSeconds > 0
            onToggled: backend.streamTimerSeconds = checked ? streamMinutes.value * 60 : 0
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("End each stream once it has been live this long")
        }
        SpinBox {
            id: streamMinutes
            objectName: "streamTimerMinutes"
            from: 1; to: 10080; editable: true
            value: backend.streamTimerSeconds > 0 ? Math.max(1, Math.round(backend.streamTimerSeconds / 60)) : 60
            onValueModified: if (streamOn.checked) backend.streamTimerSeconds = value * 60
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Minutes")
            Accessible.name: qsTr("Stream minutes")
        }
        Label { text: sheet.left(backend.streamTimerLeft) || qsTr("minutes"); color: Theme.textMuted }
    }
}
