import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtQuick.Layouts
import OmaBroadcast 1.0

// The live scene's items, top of the stack first. Click selects, the eye
// shows or hides, the lock stops the preview moving it.
ColumnLayout {
    id: panel
    spacing: 0
    signal renameRequested(string sourceId, string name)
    signal menuRequested(var row, real x, real y, var at)
    signal dockMenuRequested(Item button)
    // Folded down to its header by the arrow (or the title); the dock decides.
    property bool folded: false
    signal twirlRequested()

    readonly property var kindIcon: ({ display: "monitor", window: "app-window", region: "square", image: "image", colour: "palette",
                                       text: "type", device: "camera", media: "film", studio: "clapperboard", slideshow: "images",
                                       group: "group", "audio-input": "mic", "audio-output": "volume-2", "audio-application": "app-window",
                                       browser: "globe" })
    // Groups folded shut in the list (by item id); their items are hidden.
    property var collapsed: ({})
    property string anchorId: ""     // where a Shift-click range starts
    readonly property var rows: backend.items
    function parentOf(id) {
        for (const r of rows) if (r.id === id) return r.parent
        return ""
    }
    function shown(row) {
        for (let p = row.parent; p !== ""; p = parentOf(p)) if (collapsed[p]) return false
        return true
    }
    // Click selects; Ctrl adds or removes; Shift takes the range.
    // A field typed in commits as it loses focus, so it must lose it while what
    // it edits is still selected. Otherwise the keyboard stays where it is.
    function finishTyping() {
        const item = Window.activeFocusItem
        if (item instanceof TextInput || item instanceof TextEdit) list.forceActiveFocus()
    }
    // These run from here and not from the row: that commit remakes the rows.
    function include(id) {
        finishTyping()
        if (backend.selectedItems.indexOf(id) < 0) backend.selectedItem = id
    }
    function showRowMenu(row, at) {
        include(row.id)
        menuRequested(row, at.x, at.y, list)
    }
    function pick(row, mods) {
        finishTyping()
        let sel = backend.selectedItems.slice()
        if (mods & Qt.ControlModifier) {
            const at = sel.indexOf(row.id)
            if (at >= 0) sel.splice(at, 1); else sel.push(row.id)
        } else if ((mods & Qt.ShiftModifier) && anchorId !== "") {
            const visible = rows.filter(r => shown(r)).map(r => r.id)
            const a = visible.indexOf(anchorId), b = visible.indexOf(row.id)
            sel = a < 0 || b < 0 ? [row.id] : visible.slice(Math.min(a, b), Math.max(a, b) + 1)
        } else {
            sel = [row.id]
        }
        if (!(mods & Qt.ShiftModifier)) anchorId = row.id
        backend.selectedItems = sel
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Theme.s1
        Layout.rightMargin: Theme.s2
        implicitHeight: Theme.hNavHeader
        spacing: Theme.s1
        DockTwirl { folded: panel.folded; title: qsTr("Sources"); onClicked: panel.twirlRequested() }
        Label {
            text: qsTr("Sources")
            color: Theme.textPrimary
            font.pixelSize: Theme.fsHeading
            font.weight: Theme.wHeading
            Layout.fillWidth: true
            TapHandler { onTapped: panel.twirlRequested() }
        }
        ToolButton {
            icon.name: "arrow-up"
            enabled: backend.selectedItem !== ""
            onClicked: backend.raiseItem(backend.selectedItem, 1)
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Bring forward")
            Accessible.name: qsTr("Bring forward")
        }
        ToolButton {
            icon.name: "arrow-down"
            enabled: backend.selectedItem !== ""
            onClicked: backend.raiseItem(backend.selectedItem, -1)
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Send backward")
            Accessible.name: qsTr("Send backward")
        }
        ToolButton {
            id: addButton
            objectName: "addSourceButton"
            icon.name: "plus"
            onClicked: addMenu.popup(addButton, 0, addButton.height)
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Add a source")
            Accessible.name: qsTr("Add a source")
        }
        DockMenuButton { onRequested: (button) => panel.dockMenuRequested(button) }
    }
    Rectangle { visible: !panel.folded; Layout.fillWidth: true; implicitHeight: Theme.hairline; color: Theme.border }

    Menu {
        id: addMenu
        objectName: "addSourceMenu"
        MenuItem { text: qsTr("Display…"); icon.name: "monitor"; onTriggered: backend.addDisplay() }
        MenuItem { text: qsTr("Window…"); icon.name: "app-window"; onTriggered: backend.addWindow() }
        MenuItem { text: qsTr("Direct display…"); icon.name: "monitor"; onTriggered: backend.addDirectCapture(false) }
        MenuItem { text: qsTr("Direct window…"); icon.name: "app-window"; onTriggered: backend.addDirectCapture(true) }
        MenuItem { text: qsTr("Follow focused window"); icon.name: "app-window"; onTriggered: backend.addDirectCapture(true, true) }
        MenuItem { text: qsTr("Display region…"); icon.name: "square"; onTriggered: backend.addRegion() }
        Menu {
            id: deviceMenu
            title: qsTr("Capture device")
            Instantiator {
                model: backend.videoDevices
                delegate: MenuItem {
                    required property var modelData
                    text: modelData.description
                    icon.name: "camera"
                    onTriggered: backend.addDevice(modelData.name)
                }
                onObjectAdded: (index, object) => deviceMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => deviceMenu.removeItem(object)
            }
            MenuItem {
                text: qsTr("No cameras or capture cards found")
                enabled: false
                visible: backend.videoDevices.length === 0
                height: visible ? implicitHeight : 0
            }
        }
        MenuItem { text: qsTr("Phone (network stream)"); icon.name: "smartphone"; onTriggered: backend.addNetworkStream() }
        Menu {
            id: studioMenu
            title: qsTr("Studio")
            Instantiator {
                model: backend.scenes.filter(s => s.showable)
                delegate: MenuItem {
                    required property var modelData
                    text: modelData.name
                    icon.name: "clapperboard"
                    onTriggered: backend.addStudio(modelData.id)
                }
                onObjectAdded: (index, object) => studioMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => studioMenu.removeItem(object)
            }
            MenuItem {
                text: qsTr("No other studio to show")
                enabled: false
                visible: backend.scenes.filter(s => s.showable).length === 0
                height: visible ? implicitHeight : 0
            }
        }
        MenuSeparator {}
        AudioSourceMenu { objectName: "addMicrophoneOrInput"; title: qsTr("Microphone or audio input"); kind: "audio-input" }
        MenuItem {
            objectName: "addDesktopSoundSource"
            text: qsTr("Desktop sound")
            icon.name: "volume-2"
            enabled: !backend.desktopAdded
            onTriggered: backend.desktopAdded = true
        }
        AudioSourceMenu { title: qsTr("Audio output"); kind: "audio-output" }
        AudioSourceMenu { title: qsTr("Application audio"); kind: "audio-application" }
        MenuItem {
            objectName: "openGlobalAudioDevices"
            text: qsTr("Sound for every studio…")
            icon.name: "sliders-horizontal"
            onTriggered: globalDevicesSheet.open()
        }
        MenuSeparator {}
        MenuItem { text: qsTr("Media file…"); icon.name: "film"; onTriggered: backend.addMedia() }
        MenuItem { text: qsTr("Image…"); icon.name: "image"; onTriggered: backend.addImage() }
        MenuItem { text: qsTr("Colour"); icon.name: "palette"; onTriggered: backend.addColour() }
        MenuItem { text: qsTr("Text"); icon.name: "type"; onTriggered: backend.addText() }
        MenuItem { text: qsTr("Slideshow…"); icon.name: "images"; onTriggered: backend.addSlideshow() }
        MenuItem {
            objectName: "addPresentationSource"
            text: qsTr("Presentation…")
            icon.name: "presentation"
            visible: backend.presentationAvailable
            height: visible ? implicitHeight : 0
            onTriggered: backend.addPresentation()
        }
        MenuItem {
            objectName: "addBrowserSource"
            text: qsTr("Browser")
            icon.name: "globe"
            visible: backend.browserAvailable
            height: visible ? implicitHeight : 0
            onTriggered: backend.addBrowser()
        }
    }

    ListView {
        visible: !panel.folded
        id: list
        objectName: "sourcesList"
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: backend.items
        // Where a dragged row would land: just above or below `target`.
        property var dropTarget: null
        property bool dropBelow: false
        function dropAt(fromRow, contentY) {
            const i = list.indexAt(Theme.s4, contentY)
            const r = i >= 0 ? panel.rows[i] : null
            // Only among its own: the same studio, or the same group.
            if (!r || r.id === fromRow.id || r.parent !== fromRow.parent) { dropTarget = null; return }
            const item = list.itemAtIndex(i)
            dropTarget = r
            dropBelow = contentY > item.y + item.height / 2
            dropLine.y = (dropBelow ? item.y + item.height : item.y) - list.contentY - dropLine.height / 2
        }
        Rectangle {
            id: dropLine
            z: 10
            visible: list.dropTarget !== null
            x: Theme.s2
            width: list.width - Theme.s4
            height: Theme.selectionRing
            color: Theme.selectionOutline
        }
        delegate: ItemDelegate {
            id: row
            required property var modelData
            required property int index
            width: ListView.view.width
            // Drag a row to move it up or down the stack.
            DragHandler {
                id: dragger
                target: null
                xAxis.enabled: false
                onActiveChanged: {
                    if (active) {
                        panel.include(row.modelData.id)
                        return
                    }
                    // Cleared first: the drop rebuilds this row, and the list is out of its reach after.
                    const target = list.dropTarget, below = list.dropBelow
                    list.dropTarget = null
                    if (target) backend.dropItem(row.modelData.id, target.id, below)
                }
                onCentroidChanged: if (active) list.dropAt(row.modelData, row.y + centroid.position.y)
            }
            opacity: dragger.active ? 0.6 : 1
            visible: panel.shown(modelData)
            implicitHeight: visible ? Theme.hRailRow + Theme.s2 : 0
            highlighted: backend.selectedItems.indexOf(modelData.id) >= 0
            hoverEnabled: true
            leftPadding: Theme.s2 + modelData.depth * Theme.s4
            // A TapHandler, not onClicked: it knows which modifier keys are held.
            TapHandler {
                acceptedButtons: Qt.LeftButton
                onTapped: panel.pick(row.modelData, point.modifiers)
            }
            onDoubleClicked: panel.renameRequested(modelData.sourceId, modelData.name)
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: (point) => {
                    panel.showRowMenu(row.modelData, row.mapToItem(list, point.position.x, point.position.y))
                }
            }
            contentItem: RowLayout {
                spacing: Theme.s2
                ToolButton {
                    ToolTip.text: panel.collapsed[row.modelData.id] ? qsTr("Open group") : qsTr("Close group")
                    visible: row.modelData.group
                    icon.name: panel.collapsed[row.modelData.id] ? "chevron-right" : "chevron-down"
                    onClicked: {
                        const c = Object.assign({}, panel.collapsed)
                        c[row.modelData.id] = !c[row.modelData.id]
                        panel.collapsed = c
                    }
                    Accessible.name: panel.collapsed[row.modelData.id] ? qsTr("Open group") : qsTr("Close group")
                }
                ToolButton {
                    icon.name: row.modelData.visible ? "eye" : "eye-off"
                    onClicked: backend.setItemVisible(row.modelData.id, !row.modelData.visible)
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: row.modelData.visible ? qsTr("Hide") : qsTr("Show")
                    Accessible.name: ToolTip.text
                }
                Icon {
                    name: row.modelData.network ? "smartphone" : panel.kindIcon[row.modelData.kind] ?? "monitor"
                    color: row.modelData.live ? Theme.textSecondary : Theme.textMuted
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        text: row.modelData.name
                        color: row.modelData.visible ? Theme.textPrimary : Theme.textMuted
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        text: row.modelData.detail
                        visible: text !== ""
                        color: row.modelData.live ? Theme.textMuted
                             : row.modelData.state.startsWith("failed") ? Theme.danger : Theme.warning
                        font.pixelSize: Theme.fsCaption
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    // A source with sound shows its level here too, mixer open or not.
                    LevelMeter {
                        objectName: "sourceListMeter"
                        readonly property var sourceLevel: backend.sourceAudioLevels[row.modelData.sourceId]
                        visible: sourceLevel !== undefined
                        Layout.fillWidth: true
                        Layout.topMargin: 2
                        Layout.preferredHeight: 3
                        level: visible ? sourceLevel : 0
                        lowColour: backend.meterSettings.low || Theme.meterLow
                        midColour: backend.meterSettings.mid || Theme.meterMid
                        highColour: backend.meterSettings.high || Theme.meterHigh
                        Accessible.name: qsTr("Audio level of %1").arg(row.modelData.name)
                    }
                }
                ToolButton {
                    icon.name: row.modelData.locked ? "lock" : "lock-open"
                    opacity: row.modelData.locked || row.hovered ? 1 : 0
                    onClicked: backend.setItemLocked(row.modelData.id, !row.modelData.locked)
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: row.modelData.locked ? qsTr("Unlock") : qsTr("Lock")
                    Accessible.name: ToolTip.text
                }
            }
        }
        Label {
            anchors.fill: parent
            anchors.margins: Theme.s4
            visible: list.count === 0
            text: qsTr("This studio is empty. Add a display, a window, an image, a colour or some text with +.")
            color: Theme.textMuted
            wrapMode: Text.Wrap
        }
    }
    GlobalAudioDevicesSheet { id: globalDevicesSheet }
}
