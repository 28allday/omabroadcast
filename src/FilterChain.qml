import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// A chain of filters, top to bottom, as in the video and the audio filters
// sheets: Add filter opens a menu sorted into groups; each row has its eye
// (on or off) and its bin; a row drags up or down; right-click for the rest,
// Delete takes the selected one out. The sheet changes the chain: this only
// asks, through the signals.
ColumnLayout {
    id: chain
    spacing: Theme.s2
    property var filters: []
    property var types: []            // catalogue entries that may be added
    property var catalogue: types     // every kind a row can be, for its name: a paste can bring others
    property int selectedIndex: 0
    property int most: 32
    property string kind: "Video"     // in object names: addVideoFilter, videoFilterList…
    property bool copyable: false     // Copy, Copy all and Paste
    property bool canPaste: false
    signal selected(int index)
    signal added(string type)
    signal removed(int index)
    signal moved(int from, int to)
    signal switched(int index, bool on)
    signal copied(int index)          // −1: every filter
    signal pasted()

    function labelOf(f) { return catalogue.find(d => d.type === f.type)?.label ?? f.type }
    // The kinds sorted into the menu's groups, in the groups' order.
    readonly property var groups: {
        const by = {}
        for (const d of types) {
            if (!by[d.group]) by[d.group] = {name: d.group, order: d.groupOrder, items: []}
            by[d.group].items.push(d)
        }
        return Object.values(by).sort((a, b) => a.order - b.order)
    }
    readonly property bool full: filters.length >= most
    // Focus first: a parameter being typed commits to the filter it was typed for.
    // That commit can remake the row that asked, so these do the rest from here.
    function pick(index) {
        list.forceActiveFocus()
        selected(index)
    }
    function showRowMenu(index, at) {
        pick(index)
        rowMenu.index = index
        rowMenu.popup(list, at.x, at.y)
    }

    // Undo and redo here too (the window's own are off while a sheet is
    // open), so a filter taken out by mistake comes straight back. A text
    // field being typed in keeps Ctrl+Z for itself.
    Shortcut { sequences: [StandardKey.Undo]; enabled: chain.visible && backend.canUndo; onActivated: backend.undo() }
    Shortcut { sequences: [StandardKey.Redo]; enabled: chain.visible && backend.canRedo; onActivated: backend.redo() }

    RowLayout {
        Layout.fillWidth: true
        SectionLabel {
            text: qsTr("FILTERS")
            Layout.fillWidth: true
            Layout.topMargin: 0
        }
        Button {
            id: addButton
            objectName: "add" + chain.kind + "Filter"
            text: qsTr("Add filter")
            icon.name: "plus"
            enabled: !chain.full
            onClicked: addMenu.popup(addButton, 0, addButton.height)
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: chain.full ? qsTr("A source can have %1 filters at most").arg(chain.most)
                                     : qsTr("Add a filter at the bottom of the list")
        }
        ToolButton {
            id: moreButton
            objectName: chain.kind.toLowerCase() + "FiltersMore"
            visible: chain.copyable
            icon.name: "ellipsis-vertical"
            onClicked: moreMenu.popup(moreButton, 0, moreButton.height)
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Copy and paste filters")
            Accessible.name: ToolTip.text
        }
    }

    Menu {
        id: addMenu
        objectName: chain.kind.toLowerCase() + "FilterAddMenu"
        Instantiator {
            model: chain.groups
            delegate: Menu {
                id: groupMenu
                required property var modelData
                title: modelData.name
                Instantiator {
                    model: groupMenu.modelData.items
                    delegate: MenuItem {
                        required property var modelData
                        objectName: "add" + chain.kind + "Filter_" + modelData.type
                        text: modelData.label
                        onTriggered: chain.added(modelData.type)
                        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                        ToolTip.text: modelData.description
                    }
                    onObjectAdded: (index, object) => groupMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => groupMenu.removeItem(object)
                }
            }
            onObjectAdded: (index, object) => addMenu.insertMenu(index, object)
            onObjectRemoved: (index, object) => addMenu.removeMenu(object)
        }
    }
    Menu {
        id: moreMenu
        MenuItem {
            objectName: "copy" + chain.kind + "Filters"
            text: qsTr("Copy all filters")
            icon.name: "copy"
            enabled: chain.filters.length > 0
            onTriggered: chain.copied(-1)
        }
        MenuItem {
            objectName: "paste" + chain.kind + "Filters"
            text: qsTr("Paste filters")
            enabled: chain.canPaste
            onTriggered: chain.pasted()
        }
    }
    // One row's menu (right-click).
    Menu {
        id: rowMenu
        property int index: -1
        readonly property var filter: chain.filters[index] ?? null
        MenuItem {
            text: rowMenu.filter?.enabled ? qsTr("Turn off") : qsTr("Turn on")
            icon.name: rowMenu.filter?.enabled ? "eye-off" : "eye"
            onTriggered: chain.switched(rowMenu.index, !rowMenu.filter.enabled)
        }
        MenuItem {
            text: qsTr("Copy")
            icon.name: "copy"
            visible: chain.copyable
            height: visible ? implicitHeight : 0
            onTriggered: chain.copied(rowMenu.index)
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Move up")
            icon.name: "arrow-up"
            enabled: rowMenu.index > 0
            onTriggered: chain.moved(rowMenu.index, rowMenu.index - 1)
        }
        MenuItem {
            text: qsTr("Move down")
            icon.name: "arrow-down"
            enabled: rowMenu.index < chain.filters.length - 1
            onTriggered: chain.moved(rowMenu.index, rowMenu.index + 1)
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Remove")
            icon.name: "trash-2"
            onTriggered: chain.removed(rowMenu.index)
        }
    }

    ListView {
        id: list
        objectName: chain.kind.toLowerCase() + "FilterList"
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: chain.filters
        currentIndex: chain.selectedIndex
        focus: true
        keyNavigationEnabled: false
        ScrollBar.vertical: ScrollBar {}
        Keys.onDeletePressed: if (chain.filters.length > 0) chain.removed(chain.selectedIndex)
        Keys.onUpPressed: (event) => {
            if (event.modifiers & Qt.ControlModifier) chain.moved(chain.selectedIndex, chain.selectedIndex - 1)
            else if (chain.selectedIndex > 0) chain.selected(chain.selectedIndex - 1)
        }
        Keys.onDownPressed: (event) => {
            if (event.modifiers & Qt.ControlModifier) chain.moved(chain.selectedIndex, chain.selectedIndex + 1)
            else if (chain.selectedIndex < chain.filters.length - 1) chain.selected(chain.selectedIndex + 1)
        }
        // Where a dragged row would land: just above or below row `target`.
        property int dropTarget: -1
        property bool dropBelow: false
        function dropAt(from, contentY) {
            const i = list.indexAt(Theme.s4, contentY)
            if (i < 0 || i === from) { dropTarget = -1; return }
            const item = list.itemAtIndex(i)
            dropTarget = i
            dropBelow = contentY > item.y + item.height / 2
            dropLine.y = (dropBelow ? item.y + item.height : item.y) - list.contentY - dropLine.height / 2
        }
        Rectangle {
            id: dropLine
            z: 10
            visible: list.dropTarget >= 0
            x: Theme.s2
            width: list.width - Theme.s4
            height: Theme.selectionRing
            color: Theme.selectionOutline
        }
        delegate: ItemDelegate {
            id: row
            required property int index
            required property var modelData
            objectName: chain.kind.toLowerCase() + "FilterRow_" + index
            width: ListView.view.width
            implicitHeight: Theme.hRailRow
            highlighted: chain.selectedIndex === index
            hoverEnabled: true
            opacity: dragger.active ? 0.6 : 1
            Accessible.name: chain.labelOf(modelData) + (modelData.enabled ? "" : qsTr(" (off)"))
            onClicked: chain.pick(index)
            DragHandler {
                id: dragger
                target: null
                xAxis.enabled: false
                onActiveChanged: {
                    if (active) { chain.pick(row.index); return }
                    // Cleared first: the drop rebuilds this row.
                    const target = list.dropTarget, below = list.dropBelow, from = row.index
                    list.dropTarget = -1
                    if (target < 0) return
                    let to = target + (below ? 1 : 0)
                    if (from < to) --to
                    if (to !== from) chain.moved(from, to)
                }
                onCentroidChanged: if (active) list.dropAt(row.index, row.y + centroid.position.y)
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: (point) => {
                    chain.showRowMenu(row.index, row.mapToItem(list, point.position.x, point.position.y))
                }
            }
            contentItem: RowLayout {
                spacing: Theme.s2
                ToolButton {
                    objectName: "switch" + chain.kind + "Filter_" + row.index
                    icon.name: row.modelData.enabled ? "eye" : "eye-off"
                    onClicked: chain.switched(row.index, !row.modelData.enabled)
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: row.modelData.enabled ? qsTr("Turn off (keeps its settings)") : qsTr("Turn on")
                    Accessible.name: ToolTip.text
                }
                Label {
                    Layout.fillWidth: true
                    text: chain.labelOf(row.modelData)
                    color: row.modelData.enabled ? Theme.textPrimary : Theme.textMuted
                    elide: Text.ElideRight
                }
                ToolButton {
                    objectName: "remove" + chain.kind + "Filter_" + row.index
                    icon.name: "trash-2"
                    // Shown on the row under the pointer and the selected one.
                    opacity: row.hovered || row.highlighted ? 1 : 0
                    onClicked: chain.removed(row.index)
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: qsTr("Remove")
                    Accessible.name: qsTr("Remove %1").arg(chain.labelOf(row.modelData))
                }
            }
        }
        Label {
            anchors.fill: parent
            anchors.margins: Theme.s4
            visible: list.count === 0
            text: qsTr("No filters yet. Press Add filter to add one; they run from the top of the list down.")
            color: Theme.textMuted
            wrapMode: Text.Wrap
        }
    }
    Label {
        Layout.fillWidth: true
        visible: chain.filters.length > 1
        text: qsTr("Drag a filter up or down to change the order.")
        color: Theme.textMuted
        font.pixelSize: Theme.fsCaption
        wrapMode: Text.Wrap
    }
}
