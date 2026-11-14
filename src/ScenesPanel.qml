import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtQuick.Layouts
import OmaBroadcast 1.0

// The studios, in order, as a list or a grid of tiles. A click makes one
// live; double-click renames; right-click for the rest.
ColumnLayout {
    id: panel
    spacing: 0
    signal renameRequested(string id, string name)
    signal transitionRequested(var scene)
    signal dockMenuRequested(Item button)
    // Folded down to its header by the arrow (or the title); the dock decides.
    property bool folded: false
    signal twirlRequested()

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Theme.s1
        Layout.rightMargin: Theme.s2
        implicitHeight: Theme.hNavHeader
        spacing: Theme.s1
        DockTwirl { folded: panel.folded; title: qsTr("Studios"); onClicked: panel.twirlRequested() }
        Label {
            text: qsTr("Studios")
            color: Theme.textPrimary
            font.pixelSize: Theme.fsHeading
            font.weight: Theme.wHeading
            Layout.fillWidth: true
            TapHandler { onTapped: panel.twirlRequested() }
        }
        ToolButton {
            objectName: "studioLayoutToggle"
            icon.name: backend.studioGrid ? "list" : "layout-grid"
            onClicked: backend.studioGrid = !backend.studioGrid
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: backend.studioGrid ? qsTr("Show studios as a list") : qsTr("Show studios as a grid")
            Accessible.name: ToolTip.text
        }
        ToolButton {
            objectName: "addStudioButton"
            icon.name: "plus"
            onClicked: backend.addScene()
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Add a studio")
            Accessible.name: qsTr("Add a studio")
        }
        DockMenuButton { onRequested: (button) => panel.dockMenuRequested(button) }
    }
    Rectangle { visible: !panel.folded; Layout.fillWidth: true; implicitHeight: Theme.hairline; color: Theme.border }

    // A field typed in commits as it loses focus, so it must lose it while what
    // it edits is still selected. Otherwise the keyboard stays where it is.
    function select(id, view) {
        const item = Window.activeFocusItem
        if (item instanceof TextInput || item instanceof TextEdit) view.forceActiveFocus()
        backend.currentScene = id
    }
    function showMenu(scene, index) { sceneMenu.scene = scene; sceneMenu.index = index; sceneMenu.popup() }

    ListView {
        id: list
        objectName: "studioList"
        visible: !panel.folded && !backend.studioGrid
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: backend.scenes
        delegate: ItemDelegate {
            id: row
            required property var modelData
            required property int index
            width: ListView.view.width
            implicitHeight: Theme.hRow
            highlighted: modelData.id === backend.currentScene
            text: modelData.name
            // In studio mode the program's studio is marked; the highlighted one is being edited.
            Rectangle {
                visible: backend.studioMode && row.modelData.id === backend.programScene
                anchors.right: parent.right
                anchors.rightMargin: Theme.s3
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.s2; height: Theme.s2; radius: width / 2
                color: Theme.recording
                Accessible.name: qsTr("On the program")
            }
            onClicked: panel.select(modelData.id, list)
            onDoubleClicked: panel.renameRequested(modelData.id, modelData.name)
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: panel.showMenu(row.modelData, row.index)
            }
        }
    }

    GridView {
        id: grid
        objectName: "studioGrid"
        visible: !panel.folded && backend.studioGrid
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.margins: Theme.s2
        clip: true
        model: backend.scenes
        // Two tiles across in a narrow panel, more as it widens.
        readonly property int across: Math.max(2, Math.floor(width / (Theme.s6 * 4)))
        cellWidth: width / across
        cellHeight: Math.round(Theme.hRow * 1.6)
        delegate: Item {
            id: tile
            required property var modelData
            required property int index
            width: grid.cellWidth
            height: grid.cellHeight
            readonly property bool live: modelData.id === backend.currentScene
            Rectangle {
                anchors.fill: parent
                anchors.margins: Theme.s1
                radius: Theme.rControl
                color: tap.hovered ? Theme.controlHover : Theme.controlBg
                border.width: tile.live ? Theme.selectionRing : Theme.hairline
                Rectangle {
                    visible: backend.studioMode && tile.modelData.id === backend.programScene
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.s2
                    width: Theme.s2; height: Theme.s2; radius: width / 2
                    color: Theme.recording
                }
                border.color: tile.live ? Theme.selectionOutline : Theme.border
                Label {
                    anchors.fill: parent
                    anchors.margins: Theme.s2
                    text: tile.modelData.name
                    color: tile.live ? Theme.textPrimary : Theme.textSecondary
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.Wrap
                    elide: Text.ElideRight
                    maximumLineCount: 2
                }
            }
            HoverHandler { id: tap }
            TapHandler {
                onTapped: panel.select(tile.modelData.id, grid)
                onDoubleTapped: panel.renameRequested(tile.modelData.id, tile.modelData.name)
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: panel.showMenu(tile.modelData, tile.index)
            }
            Accessible.role: Accessible.Button
            Accessible.name: tile.modelData.name
        }
    }

    Menu {
        id: sceneMenu
        objectName: "studioMenu"
        property var scene: ({})
        property int index: 0
        // Where it is now: the list can change while the menu is open.
        readonly property int at: backend.scenes.findIndex(s => s.id === scene.id)
        MenuItem { text: backend.studioMode ? qsTr("Preview") : qsTr("Make live"); enabled: sceneMenu.scene.id !== backend.currentScene
                   onTriggered: backend.currentScene = sceneMenu.scene.id }
        MenuItem { text: qsTr("Show in the live studio"); icon.name: "clapperboard"; enabled: sceneMenu.scene.showable === true
                   onTriggered: backend.addStudio(sceneMenu.scene.id) }
        MenuSeparator {}
        MenuItem { text: qsTr("Rename…"); onTriggered: panel.renameRequested(sceneMenu.scene.id, sceneMenu.scene.name) }
        MenuItem { objectName: "studioTransitionItem"; text: qsTr("Transition…")
                   onTriggered: panel.transitionRequested(sceneMenu.scene) }
        MenuItem { objectName: "studioProjectorItem"; text: qsTr("Open a projector"); icon.name: "monitor"
                   onTriggered: backend.openProjector("studio", sceneMenu.scene.id, -1) }
        MenuItem { objectName: "studioScreenshotItem"; text: qsTr("Screenshot of the studio"); icon.name: "image"
                   onTriggered: backend.screenshot("studio", sceneMenu.scene.id) }
        MenuItem { text: qsTr("Duplicate"); icon.name: "copy"; onTriggered: backend.duplicateScene(sceneMenu.scene.id) }
        MenuSeparator {}
        MenuItem { text: qsTr("Move up"); icon.name: "arrow-up"; enabled: sceneMenu.at > 0
                   onTriggered: backend.moveScene(sceneMenu.scene.id, sceneMenu.at - 1) }
        MenuItem { text: qsTr("Move down"); icon.name: "arrow-down"; enabled: sceneMenu.at >= 0 && sceneMenu.at < backend.scenes.length - 1
                   onTriggered: backend.moveScene(sceneMenu.scene.id, sceneMenu.at + 1) }
        MenuSeparator {}
        MenuItem { text: qsTr("Remove"); icon.name: "trash-2"; enabled: backend.scenes.length > 1
                   onTriggered: backend.removeScene(sceneMenu.scene.id) }
    }
}
