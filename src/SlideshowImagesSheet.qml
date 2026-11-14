import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

Sheet {
    id: sheet
    helpTopic: "sources"
    objectName: "slideshowImagesSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: qsTr("Slideshow images")
    width: Math.min(760, parent ? parent.width - Theme.s4 : 760)
    height: Math.min(580, parent ? parent.height - Theme.s4 : 580)
    standardButtons: Dialog.Close
    // One width for the buttons under the list, so its two rows line up.
    readonly property int buttonWidth: Theme.s6 * 2 + Theme.s4
    property string sourceId: ""
    property var generation: 0
    property var entries: []
    property int selectedIndex: -1
    property bool validSource: false
    readonly property var selectedEntry: entries[selectedIndex] ?? null

    function showSource(id) {
        sourceId = id
        generation = backend.collectionGeneration()
        selectedIndex = 0
        refresh()
        if (backend.hasSlideshow(id)) open()
    }
    function refresh() {
        validSource = generation === backend.collectionGeneration() && backend.hasSlideshow(sourceId)
        if (!validSource) {
            close()
            return
        }
        entries = backend.slideshowItems(sourceId)
        selectedIndex = Math.min(Math.max(0, selectedIndex), entries.length - 1)
    }
    function move(delta) {
        const to = selectedIndex + delta
        if (selectedIndex < 0 || to < 0 || to >= entries.length) return
        const next = entries.slice()
        const entry = next.splice(selectedIndex, 1)[0]
        next.splice(to, 0, entry)
        selectedIndex = to
        backend.setSlideshowEntries(sourceId, next)
        list.positionViewAtIndex(to, ListView.Contain)
    }
    function removeSelected() {
        if (!selectedEntry) return
        const next = entries.slice()
        next.splice(selectedIndex, 1)
        backend.setSlideshowEntries(sourceId, next)
    }
    Connections {
        target: backend
        function onItemsChanged() { if (sheet.visible) sheet.refresh() }
    }
    contentItem: ColumnLayout {
        enabled: sheet.validSource
        spacing: Theme.s3
        Label {
            Layout.fillWidth: true
            text: qsTr("Images play from top to bottom. Each folder adds its readable images in name order, without subfolders. Repeated entries play again.")
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }
        RowLayout {
            Button { objectName: "slideshowAddImages"; text: qsTr("Add images…"); icon.name: "images"
                ToolTip.text: qsTr("Add one or more images")
                onClicked: backend.pickSlideshowEntries(sheet.sourceId, false) }
            Button { objectName: "slideshowAddFolder"; text: qsTr("Add folder…"); icon.name: "folder-open"
                ToolTip.text: qsTr("Add a folder: its images play in name order")
                onClicked: backend.pickSlideshowEntries(sheet.sourceId, true) }
        }
        ListView {
            id: list
            objectName: "slideshowEntriesList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: sheet.entries
            currentIndex: sheet.selectedIndex
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                required property int index
                required property var modelData
                width: list.width
                height: Theme.hRow + Theme.s4
                highlighted: sheet.selectedIndex === index
                onClicked: sheet.selectedIndex = index
                Accessible.name: modelData.path
                contentItem: RowLayout {
                    spacing: Theme.s3
                    Icon { name: modelData.folder ? "folder-open" : "image"; size: Theme.szIcon; color: Theme.textSecondary }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label { Layout.fillWidth: true; text: modelData.name; elide: Text.ElideRight; color: Theme.textPrimary }
                        Label { Layout.fillWidth: true; text: modelData.path; elide: Text.ElideMiddle; color: Theme.textMuted; font.pixelSize: Theme.fsCaption }
                    }
                    Label { visible: modelData.missing; text: qsTr("Missing"); color: Theme.warning }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: sheet.entries.length === 0
                text: qsTr("Add images or folders to start the slideshow.")
                color: Theme.textMuted
            }
        }
        RowLayout {
            Button { objectName: "slideshowMoveUp"; Layout.preferredWidth: Math.max(implicitWidth, sheet.buttonWidth); text: qsTr("Up"); enabled: sheet.selectedIndex > 0; onClicked: sheet.move(-1); ToolTip.text: qsTr("Move the selected entry earlier") }
            Button { objectName: "slideshowMoveDown"; Layout.preferredWidth: Math.max(implicitWidth, sheet.buttonWidth); text: qsTr("Down"); enabled: sheet.selectedIndex >= 0 && sheet.selectedIndex + 1 < sheet.entries.length; onClicked: sheet.move(1); ToolTip.text: qsTr("Move the selected entry later") }
            Item { Layout.fillWidth: true }
            Button { objectName: "slideshowReplaceEntry"; Layout.preferredWidth: Math.max(implicitWidth, sheet.buttonWidth); text: qsTr("Replace…"); enabled: sheet.selectedEntry !== null
                ToolTip.text: qsTr("Swap the selected entry for another image or folder")
                onClicked: backend.pickSlideshowEntries(sheet.sourceId, sheet.selectedEntry.folder, sheet.selectedIndex) }
            Button { objectName: "slideshowRemoveEntry"; Layout.preferredWidth: Math.max(implicitWidth, sheet.buttonWidth); text: qsTr("Remove"); enabled: sheet.selectedEntry !== null; onClicked: sheet.removeSelected(); ToolTip.text: qsTr("Take the selected entry out of the slideshow") }
        }
        RowLayout {
            Button { objectName: "slideshowReloadImages"; Layout.preferredWidth: Math.max(implicitWidth, sheet.buttonWidth); text: qsTr("Reload"); ToolTip.text: qsTr("Read the folders again and start from the first image")
                     onClicked: { backend.controlSlideshow(sheet.sourceId, "reload"); sheet.refresh() } }
            Label { Layout.fillWidth: true; text: qsTr("%1 entries").arg(sheet.entries.length); color: Theme.textMuted }
            Button { objectName: "slideshowUndoEdit"; Layout.preferredWidth: Math.max(implicitWidth, sheet.buttonWidth); text: qsTr("Undo"); enabled: backend.canUndo; onClicked: backend.undo(); ToolTip.text: qsTr("Undo the last change") }
            Button { objectName: "slideshowRedoEdit"; Layout.preferredWidth: Math.max(implicitWidth, sheet.buttonWidth); text: qsTr("Redo"); enabled: backend.canRedo; onClicked: backend.redo(); ToolTip.text: qsTr("Redo the change you undid") }
        }
    }
}
