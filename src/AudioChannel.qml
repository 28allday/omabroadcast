import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// One mixer channel: meter, dB gain, mute and advanced processing. Laid out
// across, it is two lines (the meter with its peak, then the gain with the
// channel's buttons), so several channels show at once.
ColumnLayout {
    id: channel
    property string audioTarget: ""
    property real level: 0
    property var meterData: ({})
    property bool vertical: backend.meterSettings.vertical ?? false
    signal resetPeakRequested()
    property real gain: 1
    property bool muted: false
    property bool live: true
    property string status: ""
    property var processing: ({mono: false, balance: 0, syncMs: 0})
    readonly property real gainDb: gain > 0 ? 20 * Math.log10(gain) : -60
    function shownGain() { return gain > 0 ? gainDb.toFixed(1) : "-inf" }
    function peakText() { return (meterData.maximumDb ?? -120) <= -120 ? "−inf" : Number(meterData.maximumDb).toFixed(1) }
    function fromDb(db) { return Math.pow(10, Math.min(26.020599913, db) / 20) }
    signal settingEdited(string key, var value, bool gesture)
    signal gainMoved(real value)
    signal muteToggled()
    signal gestureEnded()
    spacing: Theme.s1
    AudioFiltersSheet {
        id: filtersSheet
        filters: channel.processing.audioFilters ?? []
        onEdited: (filters) => channel.settingEdited("audioFilters", filters, false)
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: channel.vertical ? 160 : stereoMeters.implicitHeight
        visible: channel.live
        Item {
            visible: channel.vertical
            Layout.preferredWidth: 24
            Layout.preferredHeight: 140
            Layout.alignment: Qt.AlignTop
            Repeater {
                model: [0, -6, -20, -40, -60]
                Label {
                    required property var modelData
                    text: modelData
                    color: Theme.textMuted
                    font.pixelSize: Theme.fsLabel
                    y: -modelData/60*parent.height-height/2
                }
            }
        }
        ColumnLayout {
            id: stereoMeters
            Layout.fillWidth: !channel.vertical
            Layout.fillHeight: channel.vertical
            spacing: Theme.s1
            Grid {
                id: meters
                Layout.fillWidth: !channel.vertical
                Layout.fillHeight: false
                columns: channel.vertical ? 8 : 1
                spacing: 2
                Repeater {
                    // How many bars, not their levels: a list of levels would remake every
                    // bar at each reading.
                    model: (channel.meterData.levels ?? backend.audioChannelLabels).length
                    Item {
                        required property int index
                        width: channel.vertical ? (backend.audioChannelCount > 2 ? 20 : Theme.hMeter) : meters.width
                        height: channel.vertical ? 140 : 14
                        Label {
                            visible: !channel.vertical
                            width: 24
                            text: backend.audioChannelLabels[parent.index] ?? ""
                            color: Theme.textMuted
                            font.pixelSize: 10
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        LevelMeter {
                            x: channel.vertical ? 0 : 24
                            width: parent.width - x
                            height: channel.vertical ? parent.height : Theme.hMeter
                            anchors.verticalCenter: parent.verticalCenter
                            vertical: channel.vertical
                            level: channel.meterData.levels?.[parent.index] ?? channel.level
                            muted: channel.muted
                            clipped: channel.meterData.clipped ?? false
                            lowColour: backend.meterSettings.low || Theme.meterLow
                            midColour: backend.meterSettings.mid || Theme.meterMid
                            highColour: backend.meterSettings.high || Theme.meterHigh
                            Accessible.name: qsTr("Audio level %1").arg(backend.audioChannelLabels[parent.index] ?? "")
                        }
                    }
                }
            }
            Item {
                visible: channel.vertical
                Layout.fillWidth: true
                implicitHeight: Theme.fsLabel + 2
                Repeater {
                    model: channel.vertical ? [] : [-60, -40, -20, -6, 0]
                    Label {
                        required property var modelData
                        text: modelData
                        color: Theme.textMuted
                        font.pixelSize: Theme.fsLabel
                        x: Math.max(24, Math.min(parent.width-width, 24+(modelData+60)/60*(parent.width-24)-width/2))
                    }
                }
                Row {
                    visible: channel.vertical
                    spacing: 2
                    Repeater {
                        model: backend.audioChannelLabels
                        Label {
                            required property string modelData
                            width: backend.audioChannelCount > 2 ? 20 : Theme.hMeter
                            text: modelData
                            color: Theme.textMuted
                            font.pixelSize: 10
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
            }
        }

        Slider {
            ToolTip.text: qsTr("Volume. Type an exact level in the dB box beside it")
            objectName: "verticalGain"
            visible: channel.vertical
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignHCenter
            orientation: Qt.Vertical
            from: -60; to: 26.020599913
            value: channel.gainDb
            onMoved: channel.gainMoved(value <= from ? 0 : channel.fromDb(value))
            onPressedChanged: if (!pressed) channel.gestureEnded()
            Accessible.name: qsTr("Gain")
        }
        Item { visible: channel.vertical; Layout.fillWidth: true }
        ToolButton {
            objectName: channel.vertical ? "" : "meterPeakReset"  // the one showing has the name
            visible: !channel.vertical
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: Theme.s5 * 3
            text: channel.peakText()
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            onClicked: channel.resetPeakRequested()
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("Maximum peak in %1 — click to reset").arg(backend.meterSettings.truePeak ? "dBTP" : "dBFS")
            Accessible.name: qsTr("Reset maximum peak")
        }
    }
    Label {
        visible: !channel.live
        Layout.fillWidth: true
        text: channel.status
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        font.pixelSize: Theme.fsLabel
    }
    ToolButton {
        objectName: channel.vertical ? "meterPeakReset" : ""
        visible: channel.vertical
        Layout.fillWidth: true
        text: channel.peakText() + (backend.meterSettings.truePeak ? " dBTP" : " dBFS")
        onClicked: channel.resetPeakRequested()
        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
        ToolTip.text: qsTr("Maximum peak — click to reset")
        Accessible.name: qsTr("Reset maximum peak")
    }
    // Across: gain, mute and the channel's buttons on one line. Upright: the
    // gain is the slider beside the meter, and these wrap onto two short lines.
    GridLayout {
        Layout.fillWidth: true
        columns: channel.vertical ? 3 : 5
        columnSpacing: Theme.s1
        rowSpacing: Theme.s1
        Slider {
            ToolTip.text: qsTr("Volume. Type an exact level in the dB box beside it")
            visible: !channel.vertical
            Layout.fillWidth: true
            from: -60; to: 26.020599913
            value: channel.gainDb
            onMoved: channel.gainMoved(value <= from ? 0 : channel.fromDb(value))
            onPressedChanged: if (!pressed) channel.gestureEnded()
            Accessible.name: qsTr("Gain")
        }
        TextField {
            objectName: "gainDb"
            text: channel.shownGain()
            Layout.preferredWidth: Theme.s5 * 3
            horizontalAlignment: Text.AlignRight
            selectByMouse: true
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            Accessible.name: qsTr("Gain in decibels")
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("Gain in dB; -inf for silence")
            onEditingFinished: {
                const entry = text.trim().toLowerCase()
                const db = Number(entry)
                if (entry !== channel.shownGain()) {
                    if (entry === "-inf" || entry === "-infinity") channel.gainMoved(0)
                    else if (entry !== "" && isFinite(db)) channel.gainMoved(channel.fromDb(Math.max(-120, db)))
                    channel.gestureEnded()
                }
                text = Qt.binding(channel.shownGain)
            }
        }
        Label { text: "dB"; color: Theme.textMuted; font.pixelSize: Theme.fsLabel }
        ToolButton {
            icon.name: channel.muted ? "volume-x" : "volume-2"
            checked: channel.muted
            onClicked: channel.muteToggled()
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: channel.muted ? qsTr("Unmute") : qsTr("Mute")
            Accessible.name: channel.muted ? qsTr("Unmute") : qsTr("Mute")
        }
        // The channel's own buttons: at the end of the line across; upright, a
        // line of their own, sharing the strip's width evenly.
        RowLayout {
            Layout.columnSpan: channel.vertical ? 3 : 1
            Layout.fillWidth: channel.vertical
            spacing: Theme.s1
        ToolButton {
            Layout.fillWidth: channel.vertical
            ToolTip.text: qsTr("Monitoring, mono, balance, sync offset, tracks and push-to-talk")
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            id: advanced
            objectName: "advancedAudio"
            text: channel.vertical ? "⋯" : ""
            icon.name: channel.vertical ? "" : "sliders-horizontal"
            onClicked: advancedSheet.open()
            Accessible.name: qsTr("Advanced audio controls")
        }
        ToolButton {
            Layout.fillWidth: channel.vertical
            readonly property int count: (channel.processing.audioFilters ?? []).length
            ToolTip.text: qsTr("Noise suppression, gate, compressor, EQ and the rest of this channel's filters (%1 added)").arg(count)
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            objectName: "openAudioFilters"
            text: channel.vertical ? qsTr("FX") : (count ? String(count) : "")
            icon.name: channel.vertical ? "" : "audio-waveform"
            Accessible.name: qsTr("Audio filters")
            onClicked: filtersSheet.open()
        }
        ToolButton {
            Layout.fillWidth: channel.vertical
            objectName: "monitorSolo"
            text: channel.vertical ? qsTr("Solo") : ""
            icon.name: channel.vertical ? "" : "headphones"
            checkable: true
            checked: channel.processing.solo ?? false
            onClicked: channel.settingEdited("solo", checked, false)
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("Solo to monitor; output tracks are unchanged")
            Accessible.name: qsTr("Solo to monitor")
        }
        }
    }
    GridLayout {
        columns: channel.vertical ? 1 : 2
        visible: (channel.processing.pushToTalk ?? false) || (channel.processing.pushToMute ?? false)
        Layout.fillWidth: true
        ToolButton {
            ToolTip.text: qsTr("Hold to let this channel through while push-to-talk is on")
            objectName: "holdToTalk"
            visible: channel.processing.pushToTalk ?? false
            Layout.fillWidth: true
            text: qsTr("Hold to talk")
            checked: backend.audioPushStates[channel.audioTarget + ":talk"] ?? false
            onPressedChanged: backend.holdAudioPush(channel.audioTarget, true, pressed)
            onCanceled: backend.cancelAudioPush(channel.audioTarget)
            Component.onDestruction: backend.cancelAudioPush(channel.audioTarget)
        }
        ToolButton {
            ToolTip.text: qsTr("Hold to silence this channel")
            objectName: "holdToMute"
            visible: channel.processing.pushToMute ?? false
            Layout.fillWidth: true
            text: qsTr("Hold to mute")
            checked: backend.audioPushStates[channel.audioTarget + ":mute"] ?? false
            onPressedChanged: backend.holdAudioPush(channel.audioTarget, false, pressed)
            onCanceled: backend.cancelAudioPush(channel.audioTarget)
            Component.onDestruction: backend.cancelAudioPush(channel.audioTarget)
        }
    }
    Sheet {
        id: advancedSheet
        helpTopic: "mixer"
        objectName: "advancedAudioSheet"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Advanced audio")
        width: Math.min(400, parent ? parent.width - Theme.s4 : 400)
        height: Math.min(implicitHeight, parent ? parent.height - Theme.s4 : implicitHeight)
        standardButtons: Dialog.Close
        contentItem: ScrollView {
            objectName: "advancedAudioScroll"
            contentWidth: availableWidth
            implicitHeight: controls.implicitHeight
            clip: true
            AudioAdvancedControls {
                id: controls
                width: parent.width
                audioTarget: channel.audioTarget
                processing: channel.processing
                onSettingEdited: (key, value, gesture) => channel.settingEdited(key, value, gesture)
                onGestureEnded: channel.gestureEnded()
            }
        }
    }
}
