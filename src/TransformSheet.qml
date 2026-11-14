import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

Sheet {
    id: sheet
    helpTopic: "preview"
    objectName: "transformSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: qsTr("Edit transform")
    width: Math.min(540, parent ? parent.width - Theme.s4 : 540)
    height: Math.min(760, parent ? parent.height - Theme.s4 : 760)
    standardButtons: Dialog.Close
    property string itemId: ""
    property var generation: 0  // a quint64 counter
    function showItem(id) { itemId = id; generation = backend.collectionGeneration(); open() }
    function validate() {
        if (visible && (generation !== backend.collectionGeneration() || backend.selectedItem !== itemId || !backend.selected.id)) close()
    }
    Connections { target: backend
        function onSelectionChanged() { sheet.validate() }
        function onItemsChanged() { sheet.validate() }
    }
    contentItem: ScrollView {
        id: scroll
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        TransformControls { width: scroll.availableWidth; showEditButton: false; collapsible: false; objectName: "transformDialogControls" }
    }
}
