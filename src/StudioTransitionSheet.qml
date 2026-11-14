import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// The transition going live with one studio uses, instead of the chosen one.
Sheet {
    id: sheet
    helpTopic: "transitions"
    objectName: "studioTransitionSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 420)
    modal: true
    standardButtons: Dialog.Cancel | Dialog.Ok
    property string sceneId: ""
    readonly property var choices: [{ id: "", name: qsTr("The chosen transition") }].concat(backend.transitions)

    function showStudio(scene) {
        sceneId = scene.id
        title = qsTr("Transition to %1").arg(scene.name)
        choice.currentIndex = Math.max(0, choices.findIndex(c => c.id === (scene.transition ?? "")))
        ownDuration.checked = (scene.transitionMs ?? 0) > 0
        duration.value = (scene.transitionMs ?? 0) > 0 ? scene.transitionMs : backend.transitionDuration
        open()
    }
    onAccepted: backend.setSceneTransition(sceneId, choices[choice.currentIndex].id, ownDuration.checked ? duration.value : 0)

    contentItem: ColumnLayout {
        spacing: Theme.s2
        ComboBox {
            ToolTip.text: qsTr("The transition going live with this studio uses")
            id: choice
            objectName: "studioTransitionChoice"
            Layout.fillWidth: true
            model: sheet.choices
            textRole: "name"
            Accessible.name: qsTr("Transition")
        }
        RowLayout {
            CheckBox { id: ownDuration; objectName: "studioTransitionOwnDuration"; text: qsTr("Its own duration"); Layout.fillWidth: true
                       ToolTip.text: qsTr("Take its own time rather than the chosen duration") }
            SpinBox {
                ToolTip.text: qsTr("How long it takes, in milliseconds")
                id: duration
                objectName: "studioTransitionDuration"
                enabled: ownDuration.checked
                from: 50; to: 20000; stepSize: 50
                editable: true
                Accessible.name: qsTr("Duration")
            }
            Label { text: qsTr("ms"); color: Theme.textMuted }
        }
    }
}
