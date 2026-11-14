import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// A made transition's settings, applied as they change.
Sheet {
    id: sheet
    helpTopic: "transitions"
    objectName: "transitionSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 460)
    modal: true
    standardButtons: Dialog.Close
    property string transitionId: ""
    readonly property var entry: backend.transitions.find(t => t.id === transitionId) ?? ({})
    readonly property var set: entry.settings ?? ({})
    readonly property var kind: backend.transitionKinds.find(k => k.id === set.kind) ?? ({})
    title: entry.name ?? ""

    function showTransition(id) { transitionId = id; open() }
    function setting(key, value, merge) { backend.setTransitionSetting(transitionId, key, value, merge ? "transition." + transitionId + "." + key : "") }

    contentItem: ColumnLayout {
        spacing: Theme.s2
        RowLayout {
            Label { text: qsTr("Kind"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox {
                ToolTip.text: qsTr("What the transition does")
                objectName: "transitionKind"
                Layout.preferredWidth: Theme.s6 * 5
                model: backend.transitionKinds.filter(k => k.id !== "cut")
                textRole: "name"
                valueRole: "id"
                currentIndex: model.findIndex(k => k.id === sheet.set.kind)
                onActivated: sheet.setting("kind", currentValue, false)
                Accessible.name: qsTr("Kind")
            }
        }
        RowLayout {
            visible: sheet.kind.side === true
            Label { text: qsTr("Direction"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox {
                ToolTip.text: qsTr("Which side the next studio comes from")
                objectName: "transitionSide"
                Layout.preferredWidth: Theme.s6 * 5
                model: backend.transitionSides
                textRole: "name"
                valueRole: "id"
                currentIndex: model.findIndex(s => s.id === sheet.set.side)
                onActivated: sheet.setting("side", currentValue, false)
                Accessible.name: qsTr("Direction")
            }
        }
        RowLayout {
            visible: sheet.kind.colour === true
            Label { text: qsTr("Colour"); color: Theme.textSecondary; Layout.fillWidth: true }
            ColourField {
                ToolTip.text: qsTr("The colour shown half way")
                objectName: "transitionColour"
                Layout.preferredWidth: Theme.s6 * 5
                Layout.fillWidth: false
                value: sheet.set.colour ?? "#000000"
                onCommitted: (value) => sheet.setting("colour", value, false)
            }
        }
        RowLayout {
            visible: sheet.kind.colour === true
            Label { text: qsTr("Colour point"); color: Theme.textSecondary; Layout.fillWidth: true }
            // The slider and its value together as wide as the combos.
            RowLayout {
                Layout.preferredWidth: Theme.s6 * 5
                Layout.fillWidth: false
                spacing: Theme.s1
            Slider {
                ToolTip.text: qsTr("How far through the transition only the colour shows")
                objectName: "transitionPoint"
                Layout.fillWidth: true
                from: 0; to: 1
                value: sheet.set.point ?? 0.5
                onMoved: sheet.setting("point", value, true)
                onPressedChanged: if (!pressed) backend.endGesture()
                Accessible.name: qsTr("Colour point")
            }
            Label { text: Math.round((sheet.set.point ?? 0.5) * 100) + "%"; color: Theme.textMuted; font.family: Theme.monoFamily
                    horizontalAlignment: Text.AlignRight; Layout.preferredWidth: Theme.s5 + Theme.s3 }
            }
        }
        RowLayout {
            visible: sheet.kind.mask === true
            Label { text: qsTr("Pattern"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox {
                ToolTip.text: qsTr("The shape the next studio is revealed in")
                objectName: "transitionMask"
                Layout.preferredWidth: Theme.s6 * 5
                model: backend.transitionMasks
                textRole: "name"
                valueRole: "id"
                currentIndex: model.findIndex(m => m.id === sheet.set.mask)
                onActivated: sheet.setting("mask", currentValue, false)
                Accessible.name: qsTr("Pattern")
            }
        }
        RowLayout {
            visible: sheet.kind.mask === true
            Label { text: qsTr("Softness"); color: Theme.textSecondary; Layout.fillWidth: true }
            // The slider and its value together as wide as the combos.
            RowLayout {
                Layout.preferredWidth: Theme.s6 * 5
                Layout.fillWidth: false
                spacing: Theme.s1
            Slider {
                ToolTip.text: qsTr("How soft the edge of the pattern is")
                objectName: "transitionSoftness"
                Layout.fillWidth: true
                from: 0; to: 1
                value: sheet.set.softness ?? 0.1
                onMoved: sheet.setting("softness", value, true)
                onPressedChanged: if (!pressed) backend.endGesture()
                Accessible.name: qsTr("Softness")
            }
            Label { text: Math.round((sheet.set.softness ?? 0.1) * 100) + "%"; color: Theme.textMuted; font.family: Theme.monoFamily
                    horizontalAlignment: Text.AlignRight; Layout.preferredWidth: Theme.s5 + Theme.s3 }
            }
        }
        RowLayout {
            visible: sheet.kind.stinger === true
            Label { text: qsTr("Video"); color: Theme.textSecondary; Layout.fillWidth: true }
            RowLayout {
                Layout.preferredWidth: Theme.s6 * 5
                Layout.fillWidth: false
            Label {
                objectName: "stingerFile"
                Layout.fillWidth: true
                text: (sheet.set.file ?? "") !== "" ? sheet.set.file.split("/").pop() : qsTr("None chosen")
                color: (sheet.set.file ?? "") !== "" ? Theme.textPrimary : Theme.textMuted
                elide: Text.ElideMiddle
            }
            Button {
                ToolTip.text: qsTr("A video, best with transparency, played over the change")
                objectName: "chooseStingerFile"
                text: qsTr("Choose…")
                onClicked: backend.chooseStingerFile(sheet.transitionId)
            }
            }
        }
        RowLayout {
            visible: sheet.kind.stinger === true
            Label { text: qsTr("Change studios at"); color: Theme.textSecondary; Layout.fillWidth: true }
            RowLayout {
                Layout.preferredWidth: Theme.s6 * 5
                Layout.fillWidth: false
            SpinBox {
                Layout.fillWidth: true
                ToolTip.text: qsTr("How far into the video the program changes studio")
                objectName: "stingerCut"
                readonly property bool frames: sheet.set.cutUnit === "frames"
                from: 0; to: frames ? 100000 : 60000; stepSize: frames ? 1 : 50
                editable: true
                value: frames ? sheet.set.cutFrames ?? 15 : sheet.set.cutMs ?? 500
                onValueModified: sheet.setting(frames ? "cutFrames" : "cutMs", value, false)
                Accessible.name: qsTr("Change studios at")
            }
            ComboBox {
                ToolTip.text: qsTr("Milliseconds, or the video's own frames")
                objectName: "stingerCutUnit"
                Layout.preferredWidth: Theme.s6 * 2
                model: [qsTr("ms"), qsTr("frames")]
                readonly property var values: ["ms", "frames"]
                currentIndex: Math.max(0, values.indexOf(sheet.set.cutUnit ?? "ms"))
                onActivated: sheet.setting("cutUnit", values[currentIndex], false)
                Accessible.name: qsTr("Unit")
            }
            }
        }
        RowLayout {
            visible: sheet.kind.stinger === true
            Label { text: qsTr("Track matte"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox {
                ToolTip.text: qsTr("A black-and-white part of the video that says where the next studio shows, instead of changing at one moment")
                objectName: "stingerMatte"
                Layout.preferredWidth: Theme.s6 * 5
                model: [qsTr("None"), qsTr("Beside the picture"), qsTr("Below the picture"), qsTr("The whole video"), qsTr("Another video")]
                readonly property var values: ["none", "horizontal", "vertical", "mask", "separate"]
                currentIndex: Math.max(0, values.indexOf(sheet.set.matte ?? "none"))
                onActivated: sheet.setting("matte", values[currentIndex], false)
                Accessible.name: qsTr("Track matte")
            }
        }
        RowLayout {
            visible: sheet.kind.stinger === true && sheet.set.matte === "separate"
            Label { text: qsTr("Matte video"); color: Theme.textSecondary; Layout.fillWidth: true }
            RowLayout {
                Layout.preferredWidth: Theme.s6 * 5
                Layout.fillWidth: false
            Label {
                objectName: "stingerMatteFile"
                Layout.fillWidth: true
                text: (sheet.set.matteFile ?? "") !== "" ? sheet.set.matteFile.split("/").pop() : qsTr("None chosen")
                color: (sheet.set.matteFile ?? "") !== "" ? Theme.textPrimary : Theme.textMuted
                elide: Text.ElideMiddle
            }
            Button {
                ToolTip.text: qsTr("A black-and-white video as long as the stinger, played with it: the next studio shows where it is bright")
                objectName: "chooseStingerMatteFile"
                text: qsTr("Choose…")
                onClicked: backend.chooseStingerFile(sheet.transitionId, "matteFile")
            }
            }
        }
        CheckBox {
            ToolTip.text: qsTr("The next studio shows where the matte is dark")
            visible: sheet.kind.stinger === true && (sheet.set.matte ?? "none") !== "none"
            objectName: "stingerMatteInvert"
            text: qsTr("Invert the matte")
            checked: sheet.set.matteInvert === true
            onToggled: sheet.setting("matteInvert", checked, false)
        }
        RowLayout {
            visible: sheet.kind.stinger === true
            Label { text: qsTr("Studio sound"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox {
                ToolTip.text: qsTr("How the studios' sound changes under the video")
                objectName: "stingerAudioFade"
                Layout.preferredWidth: Theme.s6 * 5
                model: [qsTr("Crossfade"), qsTr("Out, then in at the change")]
                readonly property var values: ["crossfade", "outIn"]
                currentIndex: Math.max(0, values.indexOf(sheet.set.audioFade ?? "crossfade"))
                onActivated: sheet.setting("audioFade", values[currentIndex], false)
                Accessible.name: qsTr("Studio sound")
            }
        }
        RowLayout {
            visible: sheet.kind.stinger === true
            Label { text: qsTr("Its sound in monitoring"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox {
                ToolTip.text: qsTr("Whether you hear the video's sound in monitoring as well as on air")
                objectName: "stingerMonitor"
                Layout.preferredWidth: Theme.s6 * 5
                model: [qsTr("On air only"), qsTr("Monitoring only"), qsTr("Both")]
                readonly property var values: ["off", "only", "both"]
                currentIndex: Math.max(0, values.indexOf(sheet.set.monitor ?? "off"))
                onActivated: sheet.setting("monitor", values[currentIndex], false)
                Accessible.name: qsTr("Its sound in monitoring")
            }
        }
        CheckBox {
            ToolTip.text: qsTr("Reveal from where the pattern would end")
            visible: sheet.kind.mask === true
            objectName: "transitionInvert"
            text: qsTr("Run the pattern backwards")
            checked: sheet.set.invert === true
            onToggled: sheet.setting("invert", checked, false)
        }
    }
}
