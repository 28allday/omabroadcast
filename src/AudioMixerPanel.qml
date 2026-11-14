import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

ColumnLayout {
    id: panel
    readonly property bool vertical: backend.meterSettings.vertical ?? false
    readonly property var channels: [].concat(
        backend.desktopAdded ? [{role: "desktop", name: qsTr("Desktop"), gain: backend.desktopGain, muted: backend.desktopMuted, processing: backend.desktopAudioSettings}] : [],
        backend.microphoneAdded ? [{role: "microphone", name: qsTr("Microphone"), gain: backend.microphoneGain, muted: backend.microphoneMuted, processing: backend.microphoneAudioSettings}] : []
    ).concat(backend.globalAudioDevices).concat(backend.sourceAudioChannels
        // A camera is its picture until someone gives it a sound.
        .filter(source => !source.camera || source.audioNode)
        .map(source => Object.assign({}, source, {role: "source"})))
    function edit(row, key, value, gesture) {
        if (row.role === "source") backend.setSourceSetting(row.id, key, value, gesture)
        else if (row.role === "global") backend.setGlobalAudioDeviceSetting(row.id, key, value)
        else backend.setGlobalAudioSetting(row.role === "desktop" ? 0 : 1, key, value)
    }
    readonly property int hiddenCount: channels.filter(row => row.processing?.mixerHidden ?? false).length
    spacing: Theme.s2
    // One line of buttons, then the channels, each with its meter, so levels
    // show as soon as the mixer does; where they are heard comes after.
    RowLayout {
        Layout.fillWidth: true
        ToolButton {
            ToolTip.text: qsTr("Show or hide channels in the mixer; hidden ones keep playing, recording and responding to shortcuts")
            objectName: "openMixerChannels"
            text: panel.hiddenCount ? qsTr("Channels (%1 hidden)").arg(panel.hiddenCount) : qsTr("Channels")
            onClicked: channelsSheet.open()
        }
        Item { Layout.fillWidth: true }
        ToolButton {
            ToolTip.text: qsTr("How the meters measure and look")
            objectName: "openMeterSettings"
            text: qsTr("Meters")
            onClicked: meterSheet.open()
        }
    }
    Label {
        objectName: "noSoundNote"
        visible: backend.noSound
        Layout.fillWidth: true
        text: qsTr("No sound yet. Add it with + in Sources, or your recording will be silent.")
        wrapMode: Text.Wrap
        color: Theme.warning
    }
    GridLayout {
        objectName: "audioChannelGrid"
        Layout.fillWidth: true
        columns: panel.vertical && backend.audioChannelCount <= 2 ? 2 : 1
        columnSpacing: Theme.s3
        rowSpacing: Theme.s4
        Repeater {
            model: panel.channels.length
            delegate: ColumnLayout {
                id: strip
                required property int index
                readonly property var row: panel.channels[index] ?? ({role: "source", id: "", name: "", processing: {}})
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: panel.vertical ? 130 : 280
                visible: !(row.processing?.mixerHidden ?? false) && (row.role !== "source" || !backend.activeAudioOnly || (row.active ?? false))
                spacing: Theme.s2
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: strip.row.name
                        font.bold: true
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textSecondary
                        elide: Text.ElideRight
                    }
                    ToolButton {
                        objectName: "removeMixerChannel-" + strip.row.role
                        visible: strip.row.role !== "source"
                        icon.name: "x"
                        onClicked: {
                            if (strip.row.role === "desktop") backend.desktopAdded = false
                            else if (strip.row.role === "microphone") backend.microphoneAdded = false
                            else backend.removeGlobalAudioDevice(strip.row.id)
                        }
                        Accessible.name: qsTr("Remove %1 from mixer").arg(strip.row.name)
                        ToolTip.text: qsTr("Remove this sound. It stops being captured; add it again with + in Sources.")
                    }
                    ToolButton {
                        objectName: "hideMixerChannel-" + strip.index
                        icon.name: "eye-off"
                        onClicked: panel.edit(strip.row, "mixerHidden", true, false)
                        Accessible.name: qsTr("Hide %1 from mixer").arg(strip.row.name)
                        ToolTip.visible: hovered
                        ToolTip.delay: Theme.tooltipDelay
                        ToolTip.text: qsTr("Hide this strip; audio continues. Restore it in Channels.")
                    }
                }
                ComboBox {
                    ToolTip.text: qsTr("Which input the Microphone channel uses. Automatic follows the desktop's default")
                    id: micPicker
                    objectName: "microphonePicker"
                    visible: strip.row.role === "microphone"
                    Layout.fillWidth: true
                    model: backend.microphones
                    textRole: "label"; valueRole: "value"
                    // Only once it is made: until then indexOfValue finds nothing, and a
                    // new source remakes every strip.
                    Component.onCompleted: currentIndex = indexOfValue(backend.microphone)
                    onActivated: backend.microphone = currentValue
                    Accessible.name: qsTr("Microphone")
                    Connections {
                        target: backend
                        function onMicrophonesChanged() { micPicker.currentIndex = micPicker.indexOfValue(backend.microphone) }
                    }
                }
                AudioTargetPicker {
                    visible: (strip.row.role === "source" || strip.row.role === "global") && (strip.row.device ?? false)
                    Layout.fillWidth: true
                    kind: strip.row.audioKind ?? "audio-input"
                    choice: strip.row.audioNode ?? ""
                    choiceName: strip.row.audioLabel ?? ""
                    onChosen: (value) => panel.edit(strip.row, "audioNode", value, false)
                }
                Label {
                    visible: (strip.row.error ?? "") !== ""
                    Layout.fillWidth: true
                    text: strip.row.error ?? ""
                    wrapMode: Text.Wrap
                    color: Theme.textMuted
                }
                AudioChannel {
                    objectName: "audioChannel-" + strip.index
                    Layout.fillWidth: true
                    audioTarget: strip.row.role === "source" || strip.row.role === "global" ? strip.row.role + ":" + strip.row.id : strip.row.role
                    vertical: panel.vertical
                    meterData: strip.row.role === "desktop" ? backend.desktopMeter
                        : strip.row.role === "microphone" ? backend.microphoneMeter
                        : strip.row.role === "global" ? backend.globalAudioDeviceMeters[strip.row.id] ?? ({}) : backend.sourceAudioMeters[strip.row.id] ?? ({})
                    onResetPeakRequested: {
                        if (strip.row.role === "global") backend.resetGlobalAudioDevicePeak(strip.row.id)
                        else backend.resetMeterPeak(strip.row.role === "desktop" ? 0 : strip.row.role === "microphone" ? 1 : -1, strip.row.id ?? "")
                    }
                    processing: strip.row.processing ?? ({})
                    onSettingEdited: (key, value, gesture) => panel.edit(strip.row, key, value, gesture)
                    gain: strip.row.gain ?? 1
                    muted: strip.row.muted ?? false
                    live: strip.row.role !== "microphone" || backend.microphoneLive
                    status: strip.row.role === "microphone" ? backend.microphoneStatus : ""
                    onGainMoved: (value) => {
                        if (strip.row.role === "desktop") backend.desktopGain = value
                        else if (strip.row.role === "microphone") backend.microphoneGain = value
                        else panel.edit(strip.row, "volume", value, true)
                    }
                    onGestureEnded: backend.endGesture()
                    onMuteToggled: {
                        if (strip.row.role === "desktop") backend.desktopMuted = !backend.desktopMuted
                        else if (strip.row.role === "microphone") backend.microphoneMuted = !backend.microphoneMuted
                        else panel.edit(strip.row, "muted", !strip.row.muted, false)
                    }
                }
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        CheckBox {
            ToolTip.text: qsTr("Show only the channels the live studio uses")
            Layout.fillWidth: true
            text: qsTr("Active sources only")
            checked: backend.activeAudioOnly
            onClicked: backend.activeAudioOnly = checked
        }
    }
    SectionLabel { text: qsTr("MONITORING OUTPUT") }
    ComboBox {
        ToolTip.text: qsTr("Where you hear the monitor mix: headphones, speakers, or off")
        id: monitorPicker
        Layout.fillWidth: true
        model: backend.monitorOutputs
        textRole: "label"; valueRole: "value"
        Component.onCompleted: currentIndex = indexOfValue(backend.monitorOutput)
        onActivated: backend.monitorOutput = currentValue
        Accessible.name: qsTr("Monitoring output")
        Connections {
            target: backend
            function onMicrophonesChanged() { monitorPicker.currentIndex = monitorPicker.indexOfValue(backend.monitorOutput) }
        }
    }
    Label {
        Layout.fillWidth: true
        text: backend.monitoringStatus
        wrapMode: Text.Wrap
        color: Theme.textMuted
        font.pixelSize: Theme.fsLabel
    }
    Sheet {
        id: channelsSheet
        helpTopic: "mixer"
        objectName: "mixerChannelsSheet"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Mixer channels")
        width: Math.min(440, parent ? parent.width - Theme.s4 : 440)
        height: Math.min(implicitHeight, parent ? parent.height - Theme.s4 : implicitHeight)
        standardButtons: Dialog.Close
        contentItem: ScrollView {
            contentWidth: availableWidth
            implicitHeight: channelChoices.implicitHeight
            clip: true
            ColumnLayout {
                id: channelChoices
                width: parent.width
                spacing: Theme.s2
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Choose which channels appear in the mixer. Hidden channels keep playing and their shortcuts still work.")
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                }
                Label {
                    visible: backend.activeAudioOnly
                    Layout.fillWidth: true
                    text: qsTr("Active sources only is on. Inactive sources appear when they become active.")
                    wrapMode: Text.Wrap
                    color: Theme.textMuted
                }
                Repeater {
                    model: panel.channels.length
                    CheckBox {
                        ToolTip.text: qsTr("Show this channel in the mixer")
                        required property int index
                        readonly property var row: panel.channels[index] ?? ({})
                        objectName: "showMixerChannel-" + index
                        Layout.fillWidth: true
                        text: row.name ?? ""
                        checked: !(row.processing?.mixerHidden ?? false)
                        onClicked: panel.edit(row, "mixerHidden", !checked, false)
                        Accessible.name: qsTr("Show %1 in mixer").arg(row.name ?? "")
                    }
                }
                Button {
                    ToolTip.text: qsTr("Show every hidden channel again")
                    objectName: "showAllMixerChannels"
                    text: qsTr("Show all channels")
                    enabled: panel.hiddenCount > 0
                    onClicked: {
                        const rows = panel.channels.slice()
                        for (const row of rows)
                            if (row.processing?.mixerHidden ?? false) panel.edit(row, "mixerHidden", false, false)
                    }
                }
            }
        }
    }
    Sheet {
        id: meterSheet
        helpTopic: "mixer"
        objectName: "meterSettingsSheet"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Audio meters")
        width: Math.min(460, parent ? parent.width - Theme.s4 : 460)
        height: Math.min(implicitHeight, parent ? parent.height - Theme.s4 : implicitHeight)
        standardButtons: Dialog.Close
        contentItem: ScrollView {
            contentWidth: availableWidth
            implicitHeight: controls.implicitHeight
            clip: true
            MeterControls { id: controls; width: parent.width }
        }
    }
}
