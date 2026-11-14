import QtQuick
import QtQuick.Controls

Menu {
    id: menu
    property string kind: "audio-input"
    readonly property var targets: backend.audioSourceTargets.filter(t => t.kind === menu.kind)
    Instantiator {
        model: menu.targets
        delegate: MenuItem {
            required property var modelData
            text: modelData.label
            // An input already in use would only record everything twice.
            enabled: menu?.kind !== "audio-input" || !backend.audioInputsInUse.includes(modelData.value)
            onTriggered: backend.addAudioSource(menu.kind, modelData.value)
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object)
        onObjectRemoved: (index, object) => menu.removeItem(object)
    }
    MenuItem {
        text: qsTr("Nothing connected")
        enabled: false
        visible: menu.targets.length === 0
        height: visible ? implicitHeight : 0
    }
    MenuSeparator {}
    MenuItem { text: qsTr("Choose later"); onTriggered: backend.addAudioSource(menu.kind, "") }
}
