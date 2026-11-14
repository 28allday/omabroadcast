import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

ColumnLayout {
    id: controls
    spacing: Theme.s2
    // A page's own label column and gap (Settings ▸ Meters), so these line
    // up with its other pages; −1: as wide as the longest label.
    property real labelWidth: -1
    property int columnSpacing: Theme.s3
    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: controls.columnSpacing
        rowSpacing: Theme.s2
        Label { text: qsTr("Measurement"); color: Theme.textSecondary; Layout.preferredWidth: controls.labelWidth }
        ComboBox {
            ToolTip.text: qsTr("Sample peak reads the samples; true peak also catches peaks between them, as broadcast rules measure, for a little more processing")
            objectName: "meterType"
            Layout.fillWidth: true
            model: [qsTr("Sample peak"), qsTr("True peak")]
            currentIndex: backend.meterSettings.truePeak ? 1 : 0
            onActivated: backend.setMeterSetting("truePeak", currentIndex === 1)
            Accessible.name: qsTr("Meter measurement")
        }
        Label { text: qsTr("Decay"); color: Theme.textSecondary; Layout.preferredWidth: controls.labelWidth }
        ComboBox {
            ToolTip.text: qsTr("How fast the bars fall back after a peak")
            objectName: "meterDecay"
            Layout.fillWidth: true
            model: [qsTr("Fast — 24 dB/s"), qsTr("Medium — 12 dB/s"), qsTr("Slow — 6 dB/s")]
            currentIndex: backend.meterSettings.decay ?? 1
            onActivated: backend.setMeterSetting("decay", currentIndex)
            Accessible.name: qsTr("Meter decay")
        }
        Label { text: qsTr("Layout"); color: Theme.textSecondary; Layout.preferredWidth: controls.labelWidth }
        ComboBox {
            ToolTip.text: qsTr("Mixer strips side by side (vertical meters) or stacked (horizontal)")
            objectName: "meterLayout"
            Layout.fillWidth: true
            model: [qsTr("Horizontal"), qsTr("Vertical")]
            currentIndex: backend.meterSettings.vertical ? 1 : 0
            onActivated: backend.setMeterSetting("vertical", currentIndex === 1)
            Accessible.name: qsTr("Mixer layout")
        }
        Label { text: qsTr("Below −20 dB"); color: Theme.textSecondary; Layout.preferredWidth: controls.labelWidth }
        ColourField {
            ToolTip.text: qsTr("Bar colour for quiet levels, below −20 dB")
            objectName: "meterLowColour"
            Layout.fillWidth: true
            value: backend.meterSettings.low || Theme.meterLow
            onCommitted: (value) => backend.setMeterSetting("low", value)
        }
        Label { text: qsTr("−20 to −6 dB"); color: Theme.textSecondary; Layout.preferredWidth: controls.labelWidth }
        ColourField {
            ToolTip.text: qsTr("Bar colour from −20 to −6 dB")
            Layout.fillWidth: true
            value: backend.meterSettings.mid || Theme.meterMid
            onCommitted: (value) => backend.setMeterSetting("mid", value)
        }
        Label { text: qsTr("Above −6 dB"); color: Theme.textSecondary; Layout.preferredWidth: controls.labelWidth }
        ColourField {
            ToolTip.text: qsTr("Bar colour for loud levels, above −6 dB")
            Layout.fillWidth: true
            value: backend.meterSettings.high || Theme.meterHigh
            onCommitted: (value) => backend.setMeterSetting("high", value)
        }
    }
    Button {
        ToolTip.text: qsTr("Use the Omarchy theme's colours again")
        objectName: "resetMeterColours"
        text: qsTr("Use theme colours")
        onClicked: { backend.setMeterSetting("low", ""); backend.setMeterSetting("mid", ""); backend.setMeterSetting("high", "") }
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
        text: qsTr("Click a channel's peak reading to reset its maximum and clipping light.")
    }
}
