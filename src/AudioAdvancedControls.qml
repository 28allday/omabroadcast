import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

ColumnLayout {
    id: root
    property var processing: ({})
    // One label width for the number fields, so their boxes line up.
    FontMetrics { id: labelMetrics; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
    readonly property real fieldLabels: Math.ceil(Math.max(labelMetrics.advanceWidth(qsTr("Sync offset")),
        labelMetrics.advanceWidth(qsTr("Talk release delay")), labelMetrics.advanceWidth(qsTr("Mute release delay")))) + Theme.s2
    property string audioTarget: ""
    signal settingEdited(string key, var value, bool gesture)
    signal gestureEnded()
    Layout.fillWidth: true
    spacing: Theme.s1
    ComboBox {
        ToolTip.text: qsTr("Whether you hear this channel in the monitoring output: off, only there, or there and in the recording and stream")
        objectName: "monitorMode"
        Layout.fillWidth: true
        model: [qsTr("Monitoring off"), qsTr("Monitor only"), qsTr("Monitor and output")]
        currentIndex: root.processing.monitor ?? 0
        onActivated: root.settingEdited("monitor", currentIndex, false)
        Accessible.name: qsTr("Audio monitoring mode")
    }
    Label { text: qsTr("Output tracks"); color: Theme.textMuted; font.pixelSize: Theme.fsLabel }
    AudioTracks {
        Layout.fillWidth: true
        mask: root.processing.tracks ?? 1
        onChosen: (mask) => root.settingEdited("tracks", mask, false)
        enabled: (root.processing.monitor ?? 0) !== 1
    }
    CheckBox {
        ToolTip.text: qsTr("Mix both sides into one, for a microphone that only feeds one side")
        text: qsTr("Mono downmix")
        checked: root.processing.mono ?? false
        onClicked: root.settingEdited("mono", checked, false)
    }
    RowLayout {
        Layout.fillWidth: true
        Label { text: qsTr("L"); color: Theme.textMuted }
        Slider {
            ToolTip.text: qsTr("Shift the sound left or right")
            Layout.fillWidth: true
            from: -1; to: 1; stepSize: 0.01
            value: root.processing.balance ?? 0
            onMoved: root.settingEdited("balance", value, true)
            onPressedChanged: if (!pressed) root.gestureEnded()
            Accessible.name: qsTr("Stereo balance")
        }
        Label { text: qsTr("R"); color: Theme.textMuted }
        ToolButton {
            ToolTip.text: qsTr("Put the balance back in the middle")
            text: qsTr("Centre")
            onClicked: root.settingEdited("balance", 0, false)
            Accessible.name: qsTr("Centre stereo balance")
        }
    }
    NumField {
        ToolTip.text: qsTr("Delay the sound (positive) or bring it earlier (negative), in milliseconds, to line it up with the picture")
        Layout.fillWidth: true
        label: qsTr("Sync offset")
        labelWidth: root.fieldLabels
        suffix: "ms"
        value: root.processing.syncMs ?? 0
        onCommitted: (value) => root.settingEdited("syncMs", value, false)
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("Positive delays audio; negative advances it and adds output buffering. Range: −1000 to 2000 ms.")
        wrapMode: Text.Wrap
        color: Theme.textMuted
        font.pixelSize: Theme.fsLabel
    }
    SectionLabel { text: qsTr("HOLD CONTROLS"); Layout.topMargin: Theme.s3 }
    CheckBox {
        ToolTip.text: qsTr("The channel is silent except while you hold the talk shortcut or button")
        objectName: "enablePushToTalk"
        text: qsTr("Push-to-talk")
        checked: root.processing.pushToTalk ?? false
        onClicked: root.settingEdited("pushToTalk", checked, false)
    }
    TextField {
        ToolTip.text: qsTr("A key or key combination that holds the channel open while pressed, in this window")
        objectName: "talkShortcut"
        Layout.fillWidth: true
        enabled: root.processing.pushToTalk ?? false
        placeholderText: qsTr("Local talk shortcut, e.g. F9")
        text: root.processing.talkShortcut ?? ""
        Accessible.name: qsTr("Push-to-talk shortcut")
        onEditingFinished: { root.settingEdited("talkShortcut", text, false); text = Qt.binding(() => root.processing.talkShortcut ?? "") }
    }
    NumField {
        ToolTip.text: qsTr("How long the channel stays open after you let go, in milliseconds")
        objectName: "talkReleaseDelay"
        Layout.fillWidth: true
        enabled: root.processing.pushToTalk ?? false
        label: qsTr("Talk release delay"); suffix: "ms"; labelWidth: root.fieldLabels
        value: root.processing.talkDelayMs ?? 0
        onCommitted: (value) => root.settingEdited("talkDelayMs", value, false)
    }
    CheckBox {
        ToolTip.text: qsTr("The channel is silent while you hold the mute shortcut or button")
        objectName: "enablePushToMute"
        text: qsTr("Push-to-mute")
        checked: root.processing.pushToMute ?? false
        onClicked: root.settingEdited("pushToMute", checked, false)
    }
    TextField {
        ToolTip.text: qsTr("A key or key combination that silences the channel while pressed, in this window")
        objectName: "muteShortcut"
        Layout.fillWidth: true
        enabled: root.processing.pushToMute ?? false
        placeholderText: qsTr("Local mute shortcut, e.g. F10")
        text: root.processing.muteShortcut ?? ""
        Accessible.name: qsTr("Push-to-mute shortcut")
        onEditingFinished: { root.settingEdited("muteShortcut", text, false); text = Qt.binding(() => root.processing.muteShortcut ?? "") }
    }
    NumField {
        ToolTip.text: qsTr("How long the channel stays silent after you let go, in milliseconds")
        objectName: "muteReleaseDelay"
        Layout.fillWidth: true
        enabled: root.processing.pushToMute ?? false
        label: qsTr("Mute release delay"); suffix: "ms"; labelWidth: root.fieldLabels
        value: root.processing.muteDelayMs ?? 0
        onCommitted: (value) => root.settingEdited("muteDelayMs", value, false)
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textMuted; font.pixelSize: Theme.fsLabel
        text: qsTr("Release delays: 0–5000 ms. Talk keeps audio open after release; mute keeps it silent. The normal Mute button always wins. Local shortcuts work while this window is active, outside dialogs and text fields. Apply global shortcuts again after changing enabled controls or keys; successful global setup is restored next launch.")
    }
    Button {
        ToolTip.text: qsTr("Make the talk and mute shortcuts, and the other hotkeys, work while other apps have focus, through the desktop's shortcut dialog")
        objectName: "applyGlobalHotkeys"
        Layout.fillWidth: true
        text: qsTr("Apply global shortcuts")
        enabled: !backend.audioHotkeysBusy
        onClicked: backend.applyGlobalHotkeys()
    }
    Button {
        ToolTip.text: qsTr("Turn the desktop-wide shortcuts off")
        Layout.fillWidth: true
        text: backend.audioHotkeysBusy ? qsTr("Cancel global shortcut setup") : qsTr("Turn off global shortcuts")
        onClicked: backend.stopGlobalHotkeys()
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textMuted; font.pixelSize: Theme.fsLabel
        text: backend.audioHotkeyStatus
    }
    Label {
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
        color: Theme.textSecondary; font.pixelSize: Theme.fsLabel
        text: [backend.audioHotkeyTriggers[root.audioTarget + ":talk"] ? qsTr("Talk: %1").arg(backend.audioHotkeyTriggers[root.audioTarget + ":talk"]) : "",
               backend.audioHotkeyTriggers[root.audioTarget + ":mute"] ? qsTr("Mute: %1").arg(backend.audioHotkeyTriggers[root.audioTarget + ":mute"]) : ""].filter(value => value !== "").join(" · ")
    }

}

