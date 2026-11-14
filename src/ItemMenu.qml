import QtQuick
import QtQuick.Controls
import OmaBroadcast 1.0

// What can be done to one item: from the preview and from the Sources list.
Menu {
    id: menu
    property var item: ({})          // a row of backend.items
    readonly property bool picture: !["audio-input", "audio-output", "audio-application"].includes(item.kind)
    component PictureAction: MenuItem {
        visible: menu.picture
        height: visible ? implicitHeight : 0
    }
    signal renameRequested(string sourceId, string name)

    function show(row, x, y, at) {
        item = row
        popup(at, x, y)
    }

    MenuItem { text: qsTr("Rename…"); onTriggered: menu.renameRequested(menu.item.sourceId, menu.item.name) }
    MenuItem { text: qsTr("Copy sources"); onTriggered: backend.copySources() }
    MenuItem { text: qsTr("Paste reference"); enabled: backend.canPasteSourceReference; onTriggered: backend.pasteSources(false) }
    MenuItem { text: qsTr("Paste duplicate"); enabled: backend.canPasteSourceDuplicate; onTriggered: backend.pasteSources(true) }
    MenuItem {
        text: menu.item.kind === "window" ? qsTr("Choose window…") : qsTr("Choose display…")
        visible: menu.item.capture === true
        height: visible ? implicitHeight : 0
        onTriggered: backend.reopenSource(menu.item.sourceId)
    }
    MenuItem {
        text: qsTr("Interact…")
        icon.name: "mouse-pointer-click"
        visible: menu.item.kind === "browser"
        height: visible ? implicitHeight : 0
        onTriggered: backend.openInteract(menu.item.sourceId)
    }
    MenuItem {
        text: qsTr("Reload page")
        icon.name: "refresh-cw"
        visible: menu.item.kind === "browser"
        height: visible ? implicitHeight : 0
        onTriggered: backend.reloadBrowser(menu.item.sourceId)
    }
    MenuSeparator { visible: menu.picture; height: visible ? implicitHeight : 0 }
    PictureAction { text: qsTr("Copy placement"); onTriggered: backend.copyPlacement(menu.item.id) }
    PictureAction { text: qsTr("Paste placement"); enabled: backend.canPastePlacement; onTriggered: backend.pastePlacement() }
    MenuSeparator { visible: menu.picture; height: visible ? implicitHeight : 0 }
    PictureAction { text: qsTr("Filters…"); onTriggered: backend.editVideoFilters(menu.item.sourceId) }
    PictureAction { text: qsTr("Copy filters"); onTriggered: backend.copyVideoFilters(menu.item.sourceId) }
    PictureAction { text: qsTr("Paste filters"); enabled: backend.canPasteVideoFilters; onTriggered: backend.pasteVideoFilters(menu.item.sourceId) }
    MenuSeparator {}
    MenuItem { text: qsTr("Group"); icon.name: "group"; onTriggered: backend.groupSelected() }
    MenuItem {
        text: qsTr("Ungroup")
        visible: menu.item.group === true
        height: visible ? implicitHeight : 0
        onTriggered: backend.ungroupSelected()
    }
    MenuSeparator { visible: menu.picture; height: visible ? implicitHeight : 0 }
    PictureAction { text: qsTr("Edit transform…"); onTriggered: backend.editTransform(menu.item.id) }
    PictureAction { text: qsTr("Fit to canvas"); enabled: !menu.item.placementLocked; onTriggered: backend.fitItem(menu.item.id) }
    PictureAction { text: qsTr("Stretch to canvas"); enabled: !menu.item.placementLocked; onTriggered: backend.stretchItem(menu.item.id) }
    PictureAction { text: qsTr("Centre"); enabled: !menu.item.placementLocked; onTriggered: backend.centreItem(menu.item.id) }
    PictureAction { text: qsTr("Rotate 90° clockwise"); enabled: !menu.item.placementLocked; onTriggered: backend.rotateItem(menu.item.id, 90) }
    PictureAction { text: qsTr("Rotate 90° counterclockwise"); enabled: !menu.item.placementLocked; onTriggered: backend.rotateItem(menu.item.id, -90) }
    PictureAction { text: qsTr("Rotate 180°"); enabled: !menu.item.placementLocked; onTriggered: backend.rotateItem(menu.item.id, 180) }
    PictureAction { text: qsTr("Reset transform"); enabled: !menu.item.placementLocked; onTriggered: backend.resetItem(menu.item.id) }
    MenuSeparator {}
    MenuItem { text: qsTr("Bring forward"); onTriggered: backend.raiseItem(menu.item.id, 1) }
    MenuItem { text: qsTr("Send backward"); onTriggered: backend.raiseItem(menu.item.id, -1) }
    MenuSeparator {}
    MenuItem {
        text: menu.item.locked ? qsTr("Unlock") : qsTr("Lock")
        icon.name: menu.item.locked ? "lock-open" : "lock"
        onTriggered: backend.setItemLocked(menu.item.id, !menu.item.locked)
    }
    MenuItem {
        text: menu.item.visible ? qsTr("Hide") : qsTr("Show")
        icon.name: menu.item.visible ? "eye-off" : "eye"
        onTriggered: backend.setItemVisible(menu.item.id, !menu.item.visible)
    }
    MenuSeparator {}
    MenuSeparator {}
    MenuItem { objectName: "sourceProjectorItem"; text: qsTr("Open a projector"); icon.name: "monitor"
               onTriggered: backend.openProjector("source", menu.item.sourceId, -1) }
    MenuItem { objectName: "sourceScreenshotItem"; text: qsTr("Screenshot of the source"); icon.name: "image"
               onTriggered: backend.screenshot("source", menu.item.sourceId) }
    MenuItem { text: qsTr("Remove"); icon.name: "trash-2"; onTriggered: backend.removeItem(menu.item.id) }
}
