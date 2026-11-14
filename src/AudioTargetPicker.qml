import QtQuick
import QtQuick.Controls

// A saved choice remains visible when its device or application is absent.
ComboBox {
    ToolTip.text: qsTr("What this source captures; it reconnects when the device or app comes back")
    id: picker
    property string kind: "audio-input"
    property string choice: ""
    property string choiceName: ""
    signal chosen(string value)
    textRole: "label"
    valueRole: "value"
    model: {
        const rows = [{value: "", label: qsTr("Off")}].concat(backend.audioSourceTargets.filter(t => t.kind === picker.kind))
        if (choice !== "" && !rows.some(t => t.value === choice))
            rows.push({value: choice, label: qsTr("%1 (not connected)").arg(choiceName || qsTr("Saved source"))})
        return rows
    }
    currentIndex: model.findIndex(row => row.value === choice)
    onActivated: chosen(currentValue)
    Accessible.name: kind === "audio-application" ? qsTr("Application to capture")
                     : kind === "audio-output" ? qsTr("Audio output to capture") : qsTr("Audio input")
}
