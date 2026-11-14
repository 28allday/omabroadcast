import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform as Platform
import OmaBroadcast 1.0

ApplicationWindow {
    id: win
    width: 1440
    height: 860
    minimumWidth: 980
    minimumHeight: 560
    title: backend.recording ? "● " + backend.recordingTime + " — " + Theme.appName : Theme.appName
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsControl
    color: Theme.windowBg

    readonly property bool dialogOpen: Sheets.count > 0
    readonly property bool textEntryFocused: activeFocusItem !== null
                                             && (activeFocusItem instanceof TextInput || activeFocusItem instanceof TextEdit)
    readonly property bool compactToolbar: width < 1180

    // ── Chrome pieces ───────────────────────────────────────────────────────
    component ToolAction: Button {
        property string tip: ""
        flat: true
        implicitHeight: Theme.hToolButton
        display: win.compactToolbar && icon.name !== "" ? AbstractButton.IconOnly : AbstractButton.TextBesideIcon
        ToolTip.visible: hovered && tip !== ""
        ToolTip.delay: Theme.tooltipDelay
        ToolTip.text: tip
        Accessible.name: text
    }
    component ToolbarRule: Rectangle {
        implicitWidth: Theme.hairline
        Layout.fillHeight: true
        Layout.topMargin: Theme.s3
        Layout.bottomMargin: Theme.s3
        Layout.leftMargin: Theme.s1
        Layout.rightMargin: Theme.s1
        color: Theme.border
    }
    component StatusText: Label {
        color: Theme.textSecondary
        font.pixelSize: Theme.fsLabel
    }
    component StatusRule: Rectangle {
        implicitWidth: Theme.hairline
        implicitHeight: Theme.s4
        color: Theme.border
    }
    // "Preview" and "Program" over the two pictures in studio mode.
    component PaneLabel: Label {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: Theme.s3
        color: Theme.textSecondary
        font.pixelSize: Theme.fsLabel
        font.weight: Theme.wHeading
        font.letterSpacing: Theme.capsTracking
    }
    component Hairline: Rectangle {
        color: Theme.border
        height: Theme.hairline
    }
    // A column of docks down one side; resizable between them.
    component DockColumn: SplitView {
        orientation: Qt.Vertical
        visible: count > 0
        handle: DockHandle {}
        onResizingChanged: if (!resizing) win.saveDockSizes()
    }
    // A hairline to look at, with a few pixels either side to grab.
    component DockHandle: Rectangle {
        id: dockHandle
        implicitWidth: Theme.hairline
        implicitHeight: Theme.hairline
        color: SplitHandle.pressed || SplitHandle.hovered ? Theme.selectionOutline : Theme.border
        containmentMask: QtObject {
            function contains(point: point): bool {
                const grab = 3
                return point.x >= -grab && point.y >= -grab && point.x <= dockHandle.width + grab && point.y <= dockHandle.height + grab
            }
        }
    }
    component PanelHeader: Rectangle {
        id: panelHeader
        property alias text: heading.text
        default property alias actions: actionRow.data
        // The dock it heads: its arrow folds the dock down to this header.
        property string dockId
        Layout.fillWidth: true
        implicitHeight: Theme.hNavHeader
        color: Theme.panelBg
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.s1
            anchors.rightMargin: Theme.s2
            spacing: Theme.s1
            DockTwirl {
                folded: win.dockFolded(panelHeader.dockId)
                title: heading.text
                onClicked: win.toggleDockFolded(panelHeader.dockId)
            }
            Label {
                id: heading
                color: Theme.textPrimary
                font.pixelSize: Theme.fsHeading
                font.weight: Theme.wHeading
                Layout.fillWidth: true
                TapHandler { onTapped: win.toggleDockFolded(panelHeader.dockId) }
            }
            Row { id: actionRow; spacing: Theme.s1 }
        }
        Hairline { anchors.bottom: parent.bottom; width: parent.width }
    }

    Binding {
        target: backend
        property: "localHotkeysEnabled"
        value: win.active && !win.dialogOpen && !win.textEntryFocused
    }

    // ── Keys ────────────────────────────────────────────────────────────────
    Shortcut { sequence: "Ctrl+R"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen; onActivated: win.toggleRecording() }
    Shortcut { sequence: "Ctrl+Shift+R"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && backend.replayActive; onActivated: backend.saveReplay() }
    Shortcut { sequence: "Ctrl+Shift+D"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen; onActivated: backend.addDisplay() }
    Shortcut { sequence: "Ctrl+Shift+W"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen; onActivated: backend.addWindow() }
    Shortcut { sequence: "Ctrl+,"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen; onActivated: settingsSheet.open() }
    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen; onActivated: helpSheet.open() }
    Shortcut { sequence: "F1"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen; onActivated: helpGuide.open() }
    Shortcut { sequences: [StandardKey.Quit]; context: Qt.ApplicationShortcut; onActivated: win.quit() }
    // Text fields keep their own undo.
    Shortcut { sequence: "Ctrl+C"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !win.textEntryFocused && backend.selectedItems.length > 0
               onActivated: backend.copySources() }
    Shortcut { sequence: "Ctrl+V"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !win.textEntryFocused && backend.canPasteSourceReference
               onActivated: backend.pasteSources(false) }
    Shortcut { sequence: "Ctrl+Shift+V"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !win.textEntryFocused && backend.canPasteSourceDuplicate
               onActivated: backend.pasteSources(true) }
    Shortcut { sequence: "Ctrl+Alt+C"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !win.textEntryFocused
               onActivated: if (backend.selectedItem !== "") backend.copyPlacement(backend.selectedItem) }
    Shortcut { sequence: "Ctrl+Alt+V"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !win.textEntryFocused
               onActivated: backend.pastePlacement() }
    // Preview zoom: fit, 100 % (one canvas pixel per screen pixel), in, out.
    Shortcut { sequence: "Ctrl+0"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen; onActivated: program.fit() }
    Shortcut { sequence: "Ctrl+1"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen; onActivated: program.actualPixels() }
    Shortcut { sequences: ["Ctrl+=", "Ctrl++"]; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen
               onActivated: program.zoomAt(program.width / 2, program.height / 2, 1.25) }
    Shortcut { sequence: "Ctrl+-"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen
               onActivated: program.zoomAt(program.width / 2, program.height / 2, 0.8) }
    Shortcut { sequence: "Ctrl+G"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !win.textEntryFocused
               onActivated: backend.groupSelected() }
    Shortcut { sequence: "Ctrl+Shift+G"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !win.textEntryFocused
               onActivated: backend.ungroupSelected() }
    Shortcut { sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !win.textEntryFocused
               onActivated: backend.undo() }
    Shortcut { sequences: [StandardKey.Redo, "Ctrl+Y"]; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !win.textEntryFocused
               onActivated: backend.redo() }

    property bool quitAfterRecording: false
    Connections {
        target: backend
        function onRecordingChanged() {
            if (win.quitAfterRecording && !backend.recording && !backend.streaming) win.quit()
        }
        function onStreamingChanged() {
            if (win.quitAfterRecording && !backend.recording && !backend.streaming) win.quit()
        }
    }
    // Every start or stop from the window or the tray comes here, so the
    // confirmations apply to all of them.
    function toggleRecording() {
        if (backend.recordingStopping) return
        if (backend.recording ? backend.confirmStop : backend.confirmStart) recordConfirmSheet.open()
        else backend.toggleRecording()
    }
    function toggleStreaming() {
        if (backend.streamingStopping) return
        if (backend.streaming ? backend.confirmStop : backend.confirmStart) streamConfirmSheet.open()
        else backend.toggleStreaming()
    }
    // The output timer's countdown, after a running clock.
    function timeLeft(seconds) {
        return seconds < 0 ? "" : "  " + qsTr("(%1:%2 left)").arg(Math.floor(seconds / 60)).arg(String(seconds % 60).padStart(2, "0"))
    }
    function showWindow() { win.show(); win.raise(); win.requestActivate() }
    // Something outside the window can bring it back: the bar icon, or the tray's.
    readonly property bool canHide: backend.omarchyBarActive || (backend.trayIcon && tray.available)
    // What the tray menu and the bar icon offer.
    function command(name) {
        switch (name) {
        case "window": win.visible ? win.hide() : win.showWindow(); break
        case "record":
            if (backend.recording ? backend.confirmStop : backend.confirmStart) win.showWindow()
            win.toggleRecording()
            break
        case "stream":
            if (backend.streaming ? backend.confirmStop : backend.confirmStart) win.showWindow()
            win.toggleStreaming()
            break
        case "replay": backend.toggleReplay(); break
        case "replaySave": backend.saveReplay(); break
        case "camera": backend.toggleVirtualCamera(); break
        case "quit": win.quit(); break
        }
    }
    // Quit for real (the tray menu, or File → Quit): closing may only hide.
    property bool quitting: false
    function quit() {
        win.quitting = true
        // A window started hidden in the tray has never been made: closing it asks nothing.
        if (win.visible) win.close()
        else win.mayClose()
    }
    // Whether the program ends here. This window going ends it, whatever else is
    // open: the event loop stops without closing the others, as a closed projector
    // is taken off the list and these are to open again as they were left.
    function mayClose() {
        if (!win.quitting && backend.closeToTray && win.canHide) {
            win.hide()
            return false
        }
        if (backend.recording || backend.streaming) {
            win.quitting = false
            if (!quitAfterRecording) { win.showWindow(); quitSheet.open() }
            return false
        }
        Qt.exit(0)
        return true
    }
    onClosing: function (close) { close.accepted = win.mayClose() }

    Platform.SystemTrayIcon {
        id: tray
        objectName: "trayIcon"
        visible: backend.trayIcon && !backend.omarchyBarActive
        icon.source: "qrc:/omabroadcast.svg"
        tooltip: backend.recording ? qsTr("OmaBroadcast — recording %1").arg(backend.recordingTime) : qsTr("OmaBroadcast")
        onActivated: (reason) => {
            if (reason !== Platform.SystemTrayIcon.Context) win.command("window")
        }
        menu: Platform.Menu {
            Platform.MenuItem { text: win.visible ? qsTr("Hide window") : qsTr("Show window")
                                onTriggered: win.command("window") }
            Platform.MenuItem {
                enabled: !backend.recordingStopping
                text: backend.recordingStopping ? qsTr("Finishing recording…") : backend.recording ? qsTr("Stop recording") : qsTr("Start recording")
                onTriggered: win.command("record")
            }
            Platform.MenuItem {
                enabled: !backend.streamingStopping
                text: backend.streamingStopping ? qsTr("Ending the stream…") : backend.streaming ? qsTr("End stream") : qsTr("Go live")
                onTriggered: win.command("stream")
            }
            Platform.MenuItem {
                text: backend.replayActive ? qsTr("Stop the replay buffer") : qsTr("Start the replay buffer")
                onTriggered: win.command("replay")
            }
            Platform.MenuItem { visible: backend.replayActive; text: qsTr("Save replay"); onTriggered: win.command("replaySave") }
            Platform.MenuItem {
                text: backend.virtualCamera ? qsTr("Stop the virtual camera") : qsTr("Start the virtual camera")
                onTriggered: win.command("camera")
            }
            Platform.MenuSeparator {}
            Platform.MenuItem { text: qsTr("Quit"); onTriggered: win.command("quit") }
        }
    }

    Connections {
        target: backend
        function onBarCommand(name) { win.command(name) }
        function onSlideshowEditRequested(sourceId) { slideshowSheet.showSource(sourceId) }
        function onTransformEditRequested(itemId) { transformSheet.showItem(itemId) }
        function onDirectCaptureRequested(sourceId, window) { directCaptureSheet.showSource(sourceId, window) }
        function onPreviewReady(image) { preview.setImage(image) }
        function onProgramReady(image) { programView.setImage(image) }
        function onCaptureRegionPreview(sourceId, image, width, height) { regionSelector.showCapture(sourceId, image, width, height) }
    }

    RegionSelectionSheet { id: regionSelector }

    // ── Layout ──────────────────────────────────────────────────────────────
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Title and menus
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.hTitleBar
            color: Theme.windowBg
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.s4
                anchors.rightMargin: Theme.s3
                spacing: Theme.s3
                Label { text: Theme.appName; color: Theme.textPrimary; font.pixelSize: Theme.fsTitle; font.weight: Font.Bold }
                MenuBar {
                    Layout.fillHeight: true
                    Menu {
                        title: qsTr("File")
                        Menu {
                            id: collectionMenu
                            title: qsTr("Collection")
                            Instantiator {
                                model: backend.collections
                                delegate: MenuItem {
                                    required property var modelData
                                    text: modelData.name
                                    checkable: true
                                    checked: modelData.current
                                    onTriggered: backend.switchCollection(modelData.file)
                                }
                                onObjectAdded: (index, object) => collectionMenu.insertItem(index, object)
                                onObjectRemoved: (index, object) => collectionMenu.removeItem(object)
                            }
                            MenuSeparator {}
                            MenuItem { text: qsTr("New…"); icon.name: "plus"
                                       onTriggered: nameSheet.ask(qsTr("New collection"), qsTr("Collection"), (name) => backend.newCollection(name)) }
                            MenuItem { text: qsTr("Rename…"); icon.name: "pencil"
                                       onTriggered: nameSheet.ask(qsTr("Rename collection"), backend.collectionName, (name) => backend.renameCollection(name)) }
                            MenuItem { text: qsTr("Duplicate"); icon.name: "copy"; onTriggered: backend.duplicateCollection() }
                            MenuItem { text: qsTr("Remove"); icon.name: "trash-2"; enabled: backend.collections.length > 1
                                       onTriggered: removeCollectionSheet.open() }
                            MenuSeparator {}
                            MenuItem { text: qsTr("Import…"); onTriggered: backend.importCollection() }
                            MenuItem { text: qsTr("Export…"); onTriggered: backend.exportCollection() }
                        }
                        MenuItem { text: qsTr("Profiles…"); onTriggered: profilesSheet.open() }
                        MenuItem { text: qsTr("Find missing files…"); icon.name: "folder-open"; onTriggered: missingSheet.open() }
                        MenuSeparator {}
                        MenuItem { text: qsTr("Show recordings"); icon.name: "folder-open"; onTriggered: backend.showRecordings() }
                        MenuItem { objectName: "remuxItem"; text: qsTr("Remux recordings…"); icon.name: "film"; onTriggered: remuxSheet.open() }
                        MenuSeparator {}
                        MenuItem { text: qsTr("Settings…"); icon.name: "settings"; onTriggered: settingsSheet.open() }
                        MenuSeparator {}
                        MenuItem { text: qsTr("Quit"); onTriggered: win.quit() }
                    }
                    Menu {
                        objectName: "editMenu"
                        title: qsTr("Edit")
                        MenuItem { text: backend.canUndo ? qsTr("Undo %1").arg(backend.undoText) : qsTr("Undo"); icon.name: "undo-2"
                                   enabled: backend.canUndo; onTriggered: backend.undo() }
                        MenuItem { text: backend.canRedo ? qsTr("Redo %1").arg(backend.redoText) : qsTr("Redo"); icon.name: "redo-2"
                                   enabled: backend.canRedo; onTriggered: backend.redo() }
                        MenuSeparator {}
                        MenuItem { text: backend.selectedItems.length > 1 ? qsTr("Remove sources") : qsTr("Remove source"); icon.name: "trash-2"
                                   enabled: backend.selectedItem !== "" || backend.selectedItems.length > 0
                                   onTriggered: backend.removeSelectedItems() }
                        MenuSeparator {}
                        MenuItem { objectName: "copySourcesAction"; text: qsTr("Copy sources"); enabled: backend.selectedItems.length > 0
                                   onTriggered: backend.copySources() }
                        MenuItem { objectName: "pasteSourceReferenceAction"; text: qsTr("Paste reference"); enabled: backend.canPasteSourceReference
                                   onTriggered: backend.pasteSources(false) }
                        MenuItem { objectName: "pasteSourceDuplicateAction"; text: qsTr("Paste duplicate"); enabled: backend.canPasteSourceDuplicate
                                   onTriggered: backend.pasteSources(true) }
                        MenuSeparator {}
                        MenuItem { text: qsTr("Copy placement"); enabled: backend.selectedItem !== ""
                                   onTriggered: backend.copyPlacement(backend.selectedItem) }
                        MenuItem { text: qsTr("Paste placement"); enabled: backend.canPastePlacement && backend.selectedItems.length > 0
                                   onTriggered: backend.pastePlacement() }
                        MenuSeparator {}
                        MenuItem { text: qsTr("Group"); icon.name: "group"; enabled: backend.selectedItems.length > 0
                                   onTriggered: backend.groupSelected() }
                        MenuItem { text: qsTr("Ungroup"); enabled: backend.selectedItems.length > 0
                                   onTriggered: backend.ungroupSelected() }
                    }
                    Menu {
                        title: qsTr("Studio")
                        MenuItem { text: qsTr("Add studio"); icon.name: "plus"; onTriggered: backend.addScene() }
                        MenuItem { text: qsTr("Duplicate studio"); icon.name: "copy"; onTriggered: backend.duplicateScene(backend.currentScene) }
                        MenuItem { text: qsTr("Rename studio…"); icon.name: "pencil"
                                   onTriggered: win.renameScene(backend.currentScene) }
                        MenuItem { text: qsTr("Remove studio"); icon.name: "trash-2"; enabled: backend.scenes.length > 1
                                   onTriggered: backend.removeScene(backend.currentScene) }
                        MenuSeparator {}
                        MenuItem { text: qsTr("Add display…"); icon.name: "monitor"; onTriggered: backend.addDisplay() }
                        MenuItem { text: qsTr("Add window…"); icon.name: "app-window"; onTriggered: backend.addWindow() }
                        MenuItem { text: qsTr("Add display region…"); icon.name: "square"; onTriggered: backend.addRegion() }
                        MenuItem { text: qsTr("Add image…"); icon.name: "image"; onTriggered: backend.addImage() }
                        MenuItem { text: qsTr("Add colour"); icon.name: "palette"; onTriggered: backend.addColour() }
                        MenuItem { text: qsTr("Add text"); icon.name: "type"; onTriggered: backend.addText() }
                        MenuItem { text: qsTr("Add slideshow…"); icon.name: "images"; onTriggered: backend.addSlideshow() }
                        MenuItem { objectName: "addPresentationItem"; text: qsTr("Add presentation…"); icon.name: "presentation"
                                   visible: backend.presentationAvailable; height: visible ? implicitHeight : 0
                                   onTriggered: backend.addPresentation() }
                        MenuItem { objectName: "addBrowserItem"; text: qsTr("Add browser"); icon.name: "globe"
                                   visible: backend.browserAvailable; height: visible ? implicitHeight : 0
                                   onTriggered: backend.addBrowser() }
                    }
                    Menu {
                        title: qsTr("View")
                        MenuItem { text: qsTr("Fit preview"); icon.name: "maximize"; onTriggered: program.fit() }
                        MenuItem { text: qsTr("Actual pixels (100%)"); icon.name: "scan"; onTriggered: program.actualPixels() }
                        MenuItem { objectName: "outputPixelsItem"; text: qsTr("Output pixels"); onTriggered: program.outputPixels() }
                        MenuItem { text: qsTr("Zoom in"); icon.name: "zoom-in"; onTriggered: program.zoomAt(program.width / 2, program.height / 2, 1.25) }
                        MenuItem { text: qsTr("Zoom out"); icon.name: "zoom-out"; onTriggered: program.zoomAt(program.width / 2, program.height / 2, 0.8) }
                        MenuSeparator {}
                        MenuItem { objectName: "lockPreviewItem"; text: qsTr("Lock preview"); checkable: true; checked: backend.previewLocked
                                   onTriggered: backend.previewLocked = checked }
                        MenuItem { objectName: "showOverflowItem"; text: qsTr("Show overflow"); checkable: true; checked: backend.showOverflow
                                   onTriggered: backend.showOverflow = checked }
                        MenuItem { objectName: "contextBarItem"; text: qsTr("Show context bar"); checkable: true; checked: backend.contextBar
                                   onTriggered: backend.contextBar = checked }
                        MenuItem { objectName: "studioModeItem"; text: qsTr("Studio mode"); checkable: true; checked: backend.studioMode
                                   onTriggered: backend.studioMode = !backend.studioMode }
                        MenuSeparator {}
                        Menu {
                            title: qsTr("Open a projector")
                            MenuItem { objectName: "programProjectorItem"; text: qsTr("Program"); onTriggered: backend.openProjector("program", "", -1) }
                            MenuItem { text: qsTr("Preview"); onTriggered: backend.openProjector("preview", "", -1) }
                            MenuItem { objectName: "multiviewProjectorItem"; text: qsTr("Multiview"); onTriggered: backend.openProjector("multiview", "", -1) }
                        }
                        MenuItem { objectName: "programScreenshotItem"; text: qsTr("Screenshot of the program"); icon.name: "image"
                                   onTriggered: backend.screenshot("program") }
                        MenuItem { text: qsTr("Screenshot of the preview"); enabled: backend.studioMode
                                   onTriggered: backend.screenshot("preview") }
                        MenuSeparator {}
                        Menu {
                            id: docksMenu
                            title: qsTr("Docks")
                            Instantiator {
                                model: win.dockIds
                                delegate: MenuItem {
                                    required property string modelData
                                    objectName: "dockToggle-" + modelData
                                    text: win.dockNames[modelData]
                                    checkable: true
                                    checked: (win.laidOut, win.dockSide(modelData) !== "")
                                    enabled: !backend.docksLocked
                                    onTriggered: checked ? win.showDock(modelData) : win.placeDock(modelData, "", -1)
                                }
                                onObjectAdded: (index, object) => docksMenu.insertItem(index, object)
                                onObjectRemoved: (index, object) => docksMenu.removeItem(object)
                            }
                            MenuSeparator {}
                            MenuItem { objectName: "browserDocksItem"; text: qsTr("Custom browser docks…"); icon.name: "globe"
                                       visible: backend.webDocksAvailable; height: visible ? implicitHeight : 0
                                       onTriggered: browserDocksSheet.open() }
                            MenuItem { objectName: "lockDocksItem"; text: qsTr("Lock docks"); checkable: true; checked: backend.docksLocked
                                       onTriggered: backend.docksLocked = checked }
                            MenuItem { text: qsTr("Reset docks"); onTriggered: win.resetDocks() }
                        }
                    }
                    Menu {
                        title: qsTr("Tools")
                        MenuItem { objectName: "setupItem"; text: qsTr("Set up for…"); enabled: !backend.recording && !backend.streaming
                                   onTriggered: setupSheet.begin() }
                        MenuSeparator {}
                        MenuItem { objectName: "prompterItem"; text: qsTr("Prompter"); icon.name: "file-text"; checkable: true
                                   checked: backend.prompterOpen; onTriggered: backend.prompterOpen = checked }
                        MenuItem { objectName: "playbackItem"; text: qsTr("Play last recording"); icon.name: "film"; checkable: true
                                   checked: backend.playbackOpen; onTriggered: backend.playbackOpen = checked }
                        MenuItem { objectName: "switcherItem"; text: qsTr("Automatic studio switcher…"); onTriggered: switcherSheet.open() }
                        MenuItem { objectName: "outputTimerItem"; text: qsTr("Output timer…"); onTriggered: outputTimerSheet.open() }
                        MenuItem { objectName: "remoteControlItem"; text: qsTr("Remote control…"); onTriggered: remoteControlSheet.open() }
                        MenuItem { objectName: "scriptsItem"; text: qsTr("Scripts…"); icon.name: "file-text"; onTriggered: scriptsSheet.open() }
                        MenuItem { objectName: "hotkeysItem"; text: qsTr("Hotkeys…"); icon.name: "keyboard"; onTriggered: settingsSheet.showPage("hotkeys") }
                    }
                    Menu {
                        title: qsTr("Record")
                        MenuItem {
                            enabled: !backend.recordingStopping
                            text: backend.recordingStopping ? qsTr("Finishing recording…") : backend.recording ? qsTr("Stop recording") : qsTr("Start recording")
                            icon.name: backend.recording ? "square" : "circle"
                            onTriggered: win.toggleRecording()
                        }
                        MenuItem {
                            objectName: "pauseRecordingItem"
                            enabled: backend.recording && !backend.recordingStopping
                            text: backend.recordingPaused ? qsTr("Resume recording") : qsTr("Pause recording")
                            icon.name: backend.recordingPaused ? "play" : "pause"
                            onTriggered: backend.setRecordingPaused(!backend.recordingPaused)
                        }
                        MenuItem {
                            enabled: backend.recording && !backend.recordingStopping && !backend.recordingPaused
                            text: qsTr("Start a new file")
                            onTriggered: backend.splitRecording()
                        }
                        MenuItem {
                            enabled: backend.recording && !backend.recordingStopping && backend.chaptersAvailable
                            text: qsTr("Mark a chapter")
                            onTriggered: backend.addChapter()
                        }
                        MenuSeparator {}
                        MenuItem {
                            objectName: "replayToggleItem"
                            text: backend.replayActive ? qsTr("Stop the replay buffer") : qsTr("Start the replay buffer")
                            onTriggered: backend.toggleReplay()
                        }
                        MenuItem {
                            objectName: "saveReplayItem"
                            enabled: backend.replayActive
                            text: qsTr("Save replay")
                            icon.name: "rotate-ccw"
                            onTriggered: backend.saveReplay()
                        }
                        MenuItem { text: qsTr("Replay buffer settings…"); onTriggered: settingsSheet.showPage("replay") }
                    }
                    Menu {
                        title: qsTr("Stream")
                        MenuItem {
                            enabled: !backend.streamingStopping
                            text: backend.streamingStopping ? qsTr("Ending the stream…") : backend.streaming ? qsTr("End stream") : qsTr("Go live")
                            onTriggered: win.toggleStreaming()
                        }
                        MenuItem { text: qsTr("Stream settings…"); onTriggered: settingsSheet.showStreaming() }
                        MenuSeparator {}
                        MenuItem {
                            objectName: "virtualCameraItem"
                            text: backend.virtualCamera ? qsTr("Stop the virtual camera") : qsTr("Start the virtual camera")
                            icon.name: "camera"
                            onTriggered: backend.toggleVirtualCamera()
                        }
                        MenuItem { text: qsTr("Virtual camera settings…"); onTriggered: settingsSheet.showPage("camera") }
                    }
                    Menu {
                        title: qsTr("Help")
                        MenuItem { objectName: "helpGuideItem"; text: qsTr("Guide"); icon.name: "book-open"; onTriggered: helpGuide.open() }
                        MenuItem { text: qsTr("Keyboard shortcuts"); icon.name: "keyboard"; onTriggered: helpSheet.open() }
                        MenuItem { objectName: "logItem"; text: qsTr("Log"); icon.name: "list"; onTriggered: logSheet.showRun(false) }
                        MenuItem { objectName: "repairItem"; text: qsTr("Repair…"); onTriggered: repairSheet.open() }
                    }
                }
                Item { Layout.fillWidth: true }
            }
        }

        // Toolbar
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.hToolbar
            color: Theme.panelBg
            Hairline { anchors.top: parent.top; width: parent.width }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.s3
                anchors.rightMargin: Theme.s3
                spacing: Theme.s1

                ToolButton { icon.name: "undo-2"; enabled: backend.canUndo; onClicked: backend.undo()
                             ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                             ToolTip.text: qsTr("Undo %1  Ctrl+Z").arg(backend.undoText); Accessible.name: qsTr("Undo") }
                ToolButton { icon.name: "redo-2"; enabled: backend.canRedo; onClicked: backend.redo()
                             ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                             ToolTip.text: qsTr("Redo %1  Ctrl+Shift+Z").arg(backend.redoText); Accessible.name: qsTr("Redo") }
                ToolbarRule {}
                ToolAction { objectName: "addDisplayButton"; icon.name: "monitor"; text: qsTr("Display"); tip: qsTr("Add a display  Ctrl+Shift+D"); onClicked: backend.addDisplay() }
                ToolAction { icon.name: "app-window"; text: qsTr("Window"); tip: qsTr("Add a window  Ctrl+Shift+W"); onClicked: backend.addWindow() }
                ToolAction { icon.name: "image"; text: qsTr("Image"); tip: qsTr("Add an image"); onClicked: backend.addImage() }
                ToolAction { icon.name: "type"; text: qsTr("Text"); tip: qsTr("Add text"); onClicked: backend.addText() }
                ToolbarRule {}
                ToolAction { objectName: "studioModeButton"; icon.name: "clapperboard"; text: qsTr("Studio mode"); checkable: true
                             checked: backend.studioMode; tip: qsTr("Edit and preview a studio before taking it to the program")
                             onClicked: { backend.studioMode = checked; checked = Qt.binding(() => backend.studioMode) } }
                Item { Layout.fillWidth: true }
                Label {
                    visible: backend.streaming
                    text: backend.streamingTime + win.timeLeft(backend.streamTimerLeft)
                    color: Theme.recording
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsSection
                    Layout.rightMargin: Theme.s2
                }
                RecordButton {
                    objectName: "streamButton"
                    recording: backend.streaming
                    stopping: backend.streamingStopping
                    idleText: qsTr("Go live")
                    activeText: qsTr("End stream")
                    stoppingText: qsTr("Ending…")
                    startTip: qsTr("Start streaming")
                    stopTip: qsTr("End the stream")
                    stoppingTip: qsTr("Ending the stream")
                    shortcutText: ""
                    onClicked: win.toggleStreaming()
                }
                Label {
                    visible: backend.recording
                    text: (backend.recordingPaused ? qsTr("Paused") : backend.recordingTime) + win.timeLeft(backend.recordTimerLeft)
                    color: Theme.recording
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsSection
                    Layout.rightMargin: Theme.s2
                }
                ToolButton {
                    objectName: "virtualCameraButton"
                    icon.name: "camera"
                    checkable: true
                    checked: backend.virtualCamera
                    onClicked: { backend.toggleVirtualCamera(); checked = Qt.binding(() => backend.virtualCamera) }
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: !backend.virtualCamera ? qsTr("Start the virtual camera: a camera other apps can choose. Settings picks what it shows")
                                : backend.virtualCameraInUse ? qsTr("Virtual camera on, and an app is using it. Click to stop.")
                                : qsTr("Virtual camera on; no app is using it yet. Click to stop.")
                    Accessible.name: qsTr("Virtual camera")
                }
                ToolButton {
                    objectName: "saveReplayButton"
                    visible: backend.replayActive
                    icon.name: "rotate-ccw"
                    onClicked: backend.saveReplay()
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: qsTr("Save replay (%1)  Ctrl+Shift+R").arg(backend.replayHeld)
                    Accessible.name: qsTr("Save replay")
                }
                RecordButton {
                    objectName: "recordButton"
                    recording: backend.recording
                    stopping: backend.recordingStopping
                    onClicked: win.toggleRecording()
                }
                ToolbarRule {}
                ToolButton {
                    objectName: "settingsButton"
                    icon.name: "settings"
                    onClicked: settingsSheet.open()
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Settings  Ctrl+,")
                    Accessible.name: qsTr("Settings")
                }
            }
        }
        Hairline { Layout.fillWidth: true }
        // Quitting asks first while recording or streaming; a cancel keeps safe mode.
        SafeModeBanner { Layout.fillWidth: true; onRestartRequested: { backend.restartNormally(true); win.quit() } }

        SplitView {
            id: dockSplit
            objectName: "dockSplit"
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal
            handle: DockHandle {}
            onResizingChanged: if (!resizing) win.saveDockSizes()

            DockColumn {
                id: leftDocks
                objectName: "leftDocks"
                SplitView.preferredWidth: win.dockSize("left", Theme.wSidebar)
                SplitView.minimumWidth: Theme.wSidebar * 0.75
            }
            ColumnLayout {
                SplitView.fillWidth: true
                SplitView.minimumWidth: Theme.wSidebar
                spacing: 0
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 0
                    // The picture being edited: the program, or in studio mode the preview.
                    Rectangle {
                        id: program
                        objectName: "programPreview"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.preferredWidth: 1
                        color: Theme.pasteboard

                        readonly property real aspect: backend.canvasHeight > 0 ? backend.canvasWidth / backend.canvasHeight : 16 / 9
                        readonly property real roomW: width - Theme.s5 * 2
                        readonly property real roomH: height - Theme.s5 * 2
                        readonly property real fitW: Math.max(0, Math.min(roomW, roomH * aspect))
                        clip: true

                        // Zoom (1 = fit) and pan (offset of the canvas centre from ours).
                        property real zoom: 1
                        property real panX: 0
                        property real panY: 0
                        readonly property real canvasW: fitW * zoom
                        readonly property real percent: canvasW * Screen.devicePixelRatio / Math.max(1, backend.canvasWidth) * 100
                        function fit() { zoom = 1; panX = 0; panY = 0 }
                        function actualPixels() { zoomAt(width / 2, height / 2, 100 / percent) }
                        // One recorded pixel per screen pixel: the canvas drawn at output/canvas of 100 %.
                        function outputPixels() {
                            const want = 100 * Math.max(1, backend.outputWidth) / Math.max(1, backend.canvasWidth)
                            zoomAt(width / 2, height / 2, want / percent)
                        }
                        // Zooms by `factor`, keeping the canvas point under (px, py) where it is.
                        function zoomAt(px, py, factor) {
                            const z = Math.max(0.25, Math.min(32, zoom * factor))
                            const dx = px - (width / 2 + panX), dy = py - (height / 2 + panY)
                            panX = px - width / 2 - dx * z / zoom
                            panY = py - height / 2 - dy * z / zoom
                            zoom = z
                            if (Math.abs(zoom - 1) < 0.001 && Math.abs(panX) < 1 && Math.abs(panY) < 1) fit()
                        }
                        // Ctrl+wheel zooms; the wheel alone pans once zoomed in.
                        WheelHandler {
                            acceptedModifiers: Qt.ControlModifier
                            onWheel: (event) => program.zoomAt(point.position.x, point.position.y, Math.pow(1.0015, event.angleDelta.y))
                        }
                        WheelHandler {
                            acceptedModifiers: Qt.NoModifier
                            enabled: program.zoom > 1
                            onWheel: (event) => { program.panX += event.angleDelta.x / 2; program.panY += event.angleDelta.y / 2 }
                        }
                        // A touchpad pinch zooms about the fingers.
                        PinchHandler {
                            target: null
                            onScaleChanged: (delta) => program.zoomAt(centroid.position.x, centroid.position.y, delta)
                        }
                        // Middle-drag pans.
                        DragHandler {
                            acceptedButtons: Qt.MiddleButton
                            target: null
                            property real startX: 0
                            property real startY: 0
                            onActiveChanged: if (active) { startX = program.panX; startY = program.panY }
                            onTranslationChanged: if (active) { program.panX = startX + translation.x; program.panY = startY + translation.y }
                        }

                        Rectangle {
                            x: (program.width - width) / 2 + program.panX
                            y: (program.height - height) / 2 + program.panY
                            width: program.canvasW
                            height: program.canvasW / program.aspect
                            color: Theme.showBg
                            border.width: backend.recording ? Theme.selectionRing : 0
                            border.color: Theme.recording
                            // Handles may reach past the picture's edge.
                            clip: false

                            PreviewView {
                                id: preview
                                objectName: "studioPreview"
                                anchors.fill: parent
                                anchors.margins: parent.border.width
                                Component.onCompleted: backend.setPreviewEnabled(true)
                                onWidthChanged: sizeTimer.restart()
                                onHeightChanged: sizeTimer.restart()
                                Timer {
                                    id: sizeTimer
                                    interval: 150
                                    // Zoomed in, the picture stops at the canvas's own size.
                                    onTriggered: backend.setPreviewSize(Math.round(Math.min(preview.width, backend.canvasWidth)),
                                                                        Math.round(Math.min(preview.height, backend.canvasHeight)))
                                }
                            }

                            TransformOverlay {
                                id: overlay
                                objectName: "transformOverlay"
                                anchors.fill: parent
                                canvasWidth: backend.canvasWidth
                                canvasHeight: backend.canvasHeight
                                locked: backend.previewLocked
                                showOverflow: backend.showOverflow
                                onContextMenuRequested: (itemId, x, y) => {
                                    const row = backend.items.find(r => r.id === itemId)
                                    if (row) itemMenu.show(row, x, y, overlay)
                                }
                            }

                            ColumnLayout {
                                anchors.centerIn: parent
                                visible: backend.items.length === 0
                                spacing: Theme.s3
                                Icon { name: "monitor"; size: Theme.szIconHero; color: Theme.textMuted; Layout.alignment: Qt.AlignHCenter }
                                Label {
                                    text: qsTr("Add a display or a window to begin")
                                    color: Theme.textSecondary
                                    font.pixelSize: Theme.fsSection
                                    Layout.alignment: Qt.AlignHCenter
                                }
                                Label {
                                    text: qsTr("Ctrl+Shift+D  display    ·    Ctrl+Shift+W  window")
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fsLabel
                                    Layout.alignment: Qt.AlignHCenter
                                }
                                // The way in for someone new: the guide's first task, walked through.
                                Button {
                                    objectName: "firstRecordingWalkThrough"
                                    visible: !helpCoach.running
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.topMargin: Theme.s3
                                    flat: true
                                    icon.name: "play"
                                    text: qsTr("New here? Walk me through my first recording")
                                    ToolTip.text: qsTr("Step-by-step help that points at each button in turn")
                                    onClicked: helpCoach.start(helpGuide.topicOf("first-recording"))
                                }
                            }
                        }

                        // Zoom, always at hand (a laptop has no spare keys or wheel):
                        // out, the size (click to fit), in.
                        Rectangle {
                            objectName: "previewZoom"
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: Theme.s3
                            implicitWidth: zoomRow.implicitWidth + Theme.s1 * 2
                            implicitHeight: zoomRow.implicitHeight + Theme.s1 * 2
                            width: implicitWidth
                            height: implicitHeight
                            radius: Theme.rControl
                            color: Theme.withAlpha(Theme.panelRaised, 0.85)
                            border.width: Theme.hairline
                            border.color: Theme.border
                            RowLayout {
                                id: zoomRow
                                anchors.centerIn: parent
                                spacing: 0
                                ToolButton {
                                    objectName: "previewZoomOut"
                                    icon.name: "zoom-out"
                                    implicitWidth: Theme.hControl; implicitHeight: Theme.hControl
                                    onClicked: program.zoomAt(program.width / 2, program.height / 2, 0.8)
                                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Zoom out (Ctrl+minus)")
                                    Accessible.name: qsTr("Zoom out")
                                }
                                ToolButton {
                                    objectName: "previewZoomFit"
                                    text: Math.round(program.percent) + "%"
                                    font.family: Theme.monoFamily
                                    implicitHeight: Theme.hControl
                                    onClicked: program.fit()
                                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                                    ToolTip.text: qsTr("How big the canvas is shown. Click to fit it to the space (Ctrl+0); Ctrl+1 shows it pixel for pixel")
                                    Accessible.name: qsTr("Fit the preview")
                                }
                                ToolButton {
                                    objectName: "previewZoomIn"
                                    icon.name: "zoom-in"
                                    implicitWidth: Theme.hControl; implicitHeight: Theme.hControl
                                    onClicked: program.zoomAt(program.width / 2, program.height / 2, 1.25)
                                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Zoom in (Ctrl+plus); drag with the middle button or scroll to move about")
                                    Accessible.name: qsTr("Zoom in")
                                }
                            }
                        }
                        PaneLabel { visible: backend.studioMode; text: qsTr("Preview") }
                    }
                    StudioControls {
                        objectName: "studioControls"
                        visible: backend.studioMode
                        Layout.fillHeight: true
                        Layout.fillWidth: false
                        Layout.preferredWidth: Theme.s6 * 3
                        Layout.maximumWidth: Theme.s6 * 3
                        Layout.margins: Theme.s2
                    }
                    // Studio mode: what is on air, beside the studio being edited.
                    Rectangle {
                        id: programPane
                        objectName: "programPane"
                        visible: backend.studioMode
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.preferredWidth: 1
                        color: Theme.pasteboard
                        clip: true
                        readonly property real aspect: program.aspect
                        readonly property real fitW: Math.max(0, Math.min(width - Theme.s5 * 2, (height - Theme.s5 * 2) * aspect))
                        Rectangle {
                            anchors.centerIn: parent
                            width: programPane.fitW
                            height: programPane.fitW / programPane.aspect
                            color: Theme.showBg
                            border.width: backend.recording || backend.streaming ? Theme.selectionRing : 0
                            border.color: Theme.recording
                            PreviewView {
                                id: programView
                                objectName: "programView"
                                anchors.fill: parent
                                anchors.margins: parent.border.width
                            }
                        }
                        PaneLabel { text: qsTr("Program") }
                    }
                }
                ContextBar { visible: backend.contextBar; Layout.fillWidth: true }
            }
            DockColumn {
                id: rightDocks
                objectName: "rightDocks"
                SplitView.preferredWidth: win.dockSize("right", Theme.wInspector)
                SplitView.minimumWidth: Theme.wInspector * 0.85
            }
        }

        // Every dock, made once. A closed one waits here; layoutDocks() moves
        // the open ones into their columns, so a panel keeps its state.
        Item {
            id: dockPool
            visible: false
            // Custom browser docks: a page each, made only where Qt WebEngine is.
            Repeater {
                id: webDocks
                model: webDockModel
                delegate: Rectangle {
                    id: webDock
                    required property string dock
                    required property string name
                    required property string url
                    readonly property string dockId: dock
                    objectName: "webDock-" + dock
                    color: Theme.panelBg
                    SplitView.preferredHeight: win.foldedOr(dockId, win.dockSize(dockId, Theme.hRow * 10))
                    SplitView.minimumHeight: win.foldedOr(dockId, Theme.hNavHeader + Theme.hRow * 2)
                    SplitView.maximumHeight: win.foldedOr(dockId, Number.POSITIVE_INFINITY)
                    SplitView.fillHeight: win.fillsColumn(dockId)
                    function reload() { if (page.item) page.item.reload() }
                    // Its page, once the shared profile is there.
                    function load() {
                        if (page.status === Loader.Null && webProfile.item)
                            page.setSource("qrc:/BrowserDockView.qml", { sharedProfile: webProfile.item, address: webDock.url })
                    }
                    Component.onCompleted: load()
                    Connections { target: webProfile; function onLoaded() { webDock.load() } }
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0
                        PanelHeader {
                            dockId: webDock.dockId
                            text: webDock.name
                            DockMenuButton { onRequested: (button) => dockMenu.showFor(webDock.dockId, button) }
                        }
                        Loader {
                            id: page
                            objectName: "webDockPage"
                            visible: !win.dockFolded(webDock.dockId)
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                        }
                    }
                    onUrlChanged: if (page.item) page.item.url = url
                }
            }

            Rectangle {
                id: studiosDock
                objectName: "studiosDock"
                property string dockId: "studios"
                color: Theme.panelBg
                // As tall as the studios need, up to six rows, until resized.
                readonly property real want: Theme.hNavHeader + Theme.hairline
                                             + Theme.hRow * Math.min(6, Math.max(3, backend.scenes.length))
                SplitView.preferredHeight: win.foldedOr("studios", win.dockSize("studios", want))
                SplitView.minimumHeight: win.foldedOr("studios", Theme.hNavHeader + Theme.hRow)
                SplitView.maximumHeight: win.foldedOr("studios", Number.POSITIVE_INFINITY)
                SplitView.fillHeight: win.fillsColumn("studios")
                ScenesPanel {
                    anchors.fill: parent
                    folded: win.dockFolded("studios")
                    onTwirlRequested: win.toggleDockFolded("studios")
                    onRenameRequested: (id, name) => win.renameScene(id)
                    onTransitionRequested: (scene) => studioTransitionSheet.showStudio(scene)
                    onDockMenuRequested: (button) => dockMenu.showFor("studios", button)
                }
            }
            Rectangle {
                id: transitionsDock
                objectName: "transitionsDock"
                property string dockId: "transitions"
                color: Theme.panelBg
                readonly property real want: Theme.hNavHeader + Theme.hairline + Theme.hRow * 3 + Theme.s3 * 2
                SplitView.preferredHeight: win.foldedOr("transitions", win.dockSize("transitions", want))
                SplitView.minimumHeight: win.foldedOr("transitions", Theme.hNavHeader + Theme.hRow)
                SplitView.maximumHeight: win.foldedOr("transitions", Number.POSITIVE_INFINITY)
                SplitView.fillHeight: win.fillsColumn("transitions")
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    PanelHeader {
                        dockId: "transitions"
                        text: qsTr("Transitions")
                        DockMenuButton { onRequested: (button) => dockMenu.showFor("transitions", button) }
                    }
                    TransitionControls {
                        visible: !win.dockFolded("transitions")
                        Layout.fillWidth: true
                        Layout.margins: Theme.s3
                        onPropertiesRequested: (id) => transitionSheet.showTransition(id)
                        onRenameRequested: (id, name) => nameSheet.ask(qsTr("Rename transition"), name, (n) => backend.renameTransition(id, n))
                    }
                    Item { Layout.fillHeight: true }
                }
            }
            Rectangle {
                id: sourcesDock
                objectName: "sourcesDock"
                property string dockId: "sources"
                color: Theme.panelBg
                SplitView.preferredHeight: win.foldedOr("sources", win.dockSize("sources", Theme.hRow * 8))
                SplitView.minimumHeight: win.foldedOr("sources", Theme.hNavHeader + Theme.hRow)
                SplitView.maximumHeight: win.foldedOr("sources", Number.POSITIVE_INFINITY)
                SplitView.fillHeight: win.fillsColumn("sources")
                SourcesPanel {
                    anchors.fill: parent
                    folded: win.dockFolded("sources")
                    onTwirlRequested: win.toggleDockFolded("sources")
                    onRenameRequested: (sourceId, name) => win.renameSource(sourceId, name)
                    onMenuRequested: (row, x, y, at) => itemMenu.show(row, x, y, at)
                    onDockMenuRequested: (button) => dockMenu.showFor("sources", button)
                }
            }
            Rectangle {
                id: inspectorDock
                objectName: "inspectorDock"
                property string dockId: "inspector"
                color: Theme.panelBg
                SplitView.preferredHeight: win.foldedOr("inspector", win.dockSize("inspector", Theme.hRow * 14))
                SplitView.minimumHeight: win.foldedOr("inspector", Theme.hNavHeader + Theme.hRow * 2)
                SplitView.maximumHeight: win.foldedOr("inspector", Number.POSITIVE_INFINITY)
                SplitView.fillHeight: win.fillsColumn("inspector")
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    PanelHeader {
                        dockId: "inspector"
                        text: qsTr("Source")
                        DockMenuButton { onRequested: (button) => dockMenu.showFor("inspector", button) }
                    }
                    Label {
                        visible: backend.selectedItem === "" && !win.dockFolded("inspector")
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.margins: Theme.s4
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.Wrap
                        text: qsTr("Select a source to see its settings.")
                        color: Theme.textMuted
                    }
                    ScrollView {
                        id: rightScroll
                        objectName: "sourceInspectorScroll"
                        visible: backend.selectedItem !== "" && !win.dockFolded("inspector")
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        contentWidth: availableWidth
                        ColumnLayout {
                            width: rightScroll.availableWidth - Theme.s4 * 2
                            x: Theme.s4
                            spacing: Theme.s2
                            Item { implicitHeight: Theme.s1 }
                            ItemInspector {
                                visible: backend.selectedItem !== ""
                                Layout.fillWidth: true
                            }
                            Item { implicitHeight: Theme.s4 }
                        }
                    }
                }
            }
            Rectangle {
                id: mixerDock
                objectName: "mixerDock"
                property string dockId: "mixer"
                color: Theme.panelBg
                SplitView.preferredHeight: win.foldedOr("mixer", win.dockSize("mixer", Theme.hRow * 10))
                SplitView.minimumHeight: win.foldedOr("mixer", Theme.hNavHeader + Theme.hRow * 2)
                SplitView.maximumHeight: win.foldedOr("mixer", Number.POSITIVE_INFINITY)
                SplitView.fillHeight: win.fillsColumn("mixer")
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    PanelHeader {
                        dockId: "mixer"
                        text: qsTr("Audio mixer")
                        DockMenuButton { onRequested: (button) => dockMenu.showFor("mixer", button) }
                    }
                    ScrollView {
                        id: mixerScroll
                        objectName: "mixerScroll"
                        visible: !win.dockFolded("mixer")
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        contentWidth: availableWidth
                        ColumnLayout {
                            width: mixerScroll.availableWidth - Theme.s4 * 2
                            x: Theme.s4
                            spacing: Theme.s2
                            Item { implicitHeight: Theme.s1 }
                            AudioMixerPanel { Layout.fillWidth: true }
                            Item { implicitHeight: Theme.s4 }
                        }
                    }
                }
            }
            Rectangle {
                id: statsDock
                objectName: "statsDock"
                property string dockId: "stats"
                color: Theme.panelBg
                SplitView.preferredHeight: win.foldedOr("stats", win.dockSize("stats", Theme.hRow * 12))
                SplitView.minimumHeight: win.foldedOr("stats", Theme.hNavHeader + Theme.hRow * 2)
                SplitView.maximumHeight: win.foldedOr("stats", Number.POSITIVE_INFINITY)
                SplitView.fillHeight: win.fillsColumn("stats")
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    PanelHeader {
                        dockId: "stats"
                        text: qsTr("Stats")
                        DockMenuButton { onRequested: (button) => dockMenu.showFor("stats", button) }
                    }
                    ScrollView {
                        id: statsScroll
                        objectName: "statsScroll"
                        visible: !win.dockFolded("stats")
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        contentWidth: availableWidth
                        ColumnLayout {
                            width: statsScroll.availableWidth - Theme.s4 * 2
                            x: Theme.s4
                            spacing: Theme.s2
                            StatsPanel { Layout.fillWidth: true }
                            Item { implicitHeight: Theme.s4 }
                        }
                    }
                }
            }
        }

        // Footer
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.hStatusBar
            color: Theme.windowBg
            Hairline { anchors.top: parent.top; width: parent.width }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.s4
                anchors.rightMargin: Theme.s3
                spacing: Theme.s3
                StatusText {
                    text: backend.recording ? qsTr("Recording") : qsTr("Ready")
                    color: backend.recording ? Theme.recording : Theme.textPrimary
                    font.weight: Theme.wHeading
                }
                StatusRule {}
                StatusText {
                    text: backend.status
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                // Files the collection cannot find, and the way to them.
                ToolButton {
                    ToolTip.text: qsTr("Files this collection uses that are not on this computer: find them")
                    visible: backend.missingFiles.length > 0
                    text: Theme.plural(backend.missingFiles.length, qsTr("missing file"), qsTr("missing files"))
                    icon.name: "file-text"
                    onClicked: missingSheet.open()
                }
                StatusRule { visible: backend.missingFiles.length > 0 }
                StatusText { text: backend.profileName + " · " + backend.collectionName; color: Theme.textMuted }
                StatusRule {}
                StatusText { text: backend.canvasWidth + " × " + backend.canvasHeight; color: Theme.textMuted }
                StatusRule {}
                StatusText { text: backend.statsLine; color: Theme.textMuted; font.family: Theme.monoFamily }
                StatusRule { visible: backend.engineName !== "" }
                // The GPU drawing the picture, and when the battery chose it, so.
                StatusText {
                    objectName: "engineInUse"
                    visible: backend.engineName !== ""
                    text: backend.engineOnBattery ? qsTr("%1 · on battery").arg(backend.engineName) : backend.engineName
                    color: backend.engineOnBattery ? Theme.warning : Theme.textMuted
                }
            }
        }
    }

    Sheet {
        id: quitSheet
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s5 * 2, 460)
        modal: true
        title: backend.streaming ? qsTr("Still live") : qsTr("Still recording")
        standardButtons: Dialog.Cancel | Dialog.Discard
        Component.onCompleted: standardButton(Dialog.Discard).text = qsTr("Stop and quit")
        onRejected: backend.restartNormally(false)
        onDiscarded: {
            win.quitAfterRecording = true
            if (backend.streaming) backend.toggleStreaming()
            backend.stopRecording()
            close()
            Qt.callLater(win.quit)
        }
        contentItem: Label {
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            text: backend.streaming && backend.recording ? qsTr("The stream ends, the recording stops and the file so far is kept.")
                : backend.streaming ? qsTr("The stream ends.") : qsTr("The recording stops and the file so far is kept.")
        }
    }

    // Record pressed and nothing started: the reason, where it can be seen.
    Connections {
        target: backend
        function onRecordingRefused(message) {
            recordRefusedSheet.message = message
            win.showWindow()
            recordRefusedSheet.open()
        }
    }
    Sheet {
        id: recordRefusedSheet
        objectName: "recordRefusedSheet"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s5 * 2, 480)
        modal: true
        property string message: ""
        title: qsTr("Can't start recording")
        standardButtons: Dialog.Close | Dialog.Apply
        Component.onCompleted: standardButton(Dialog.Apply).text = qsTr("Open Settings")
        onApplied: { close(); settingsSheet.showPage("recording") }
        contentItem: Label {
            objectName: "recordRefusedText"
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            text: recordRefusedSheet.message
        }
    }

    Sheet {
        id: recordConfirmSheet
        objectName: "recordConfirmSheet"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s5 * 2, 460)
        modal: true
        // Fixed when it opens, so the buttons can't swap meaning underneath you.
        property bool stopping: false
        onAboutToShow: stopping = backend.recording
        title: stopping ? qsTr("Stop recording?") : qsTr("Start recording?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: if (backend.recording === stopping) backend.toggleRecording()
        contentItem: Label {
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            text: recordConfirmSheet.stopping ? qsTr("The file is finished and closed.") : qsTr("Recording starts with the live studio.")
        }
    }

    Sheet {
        id: streamConfirmSheet
        objectName: "streamConfirmSheet"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s5 * 2, 460)
        modal: true
        property bool stopping: false
        onAboutToShow: stopping = backend.streaming
        title: stopping ? qsTr("End the stream?") : qsTr("Go live?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: if (backend.streaming === stopping) backend.toggleStreaming()
        contentItem: Label {
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            text: streamConfirmSheet.stopping ? qsTr("Viewers see the stream end.")
                                              : qsTr("The live studio goes out to every stream switched on in Settings.")
        }
    }

    // ── Docks ──────────────────────────────────────────────────────────────
    readonly property var builtInDocks: ["studios", "transitions", "sources", "inspector", "mixer", "stats"]
    // Custom browser docks follow the built-in ones, where they can be shown.
    readonly property var webDockList: backend.webDocksAvailable ? backend.browserDocks : []
    readonly property var dockIds: builtInDocks.concat(webDockList.map(d => d.id))
    // The same, read now (bindings may not have caught up inside a change).
    function knownDocks() { return builtInDocks.concat((backend.webDocksAvailable ? backend.browserDocks : []).map(d => d.id)) }
    readonly property var dockNames: {
        const names = { studios: qsTr("Studios"), transitions: qsTr("Transitions"), sources: qsTr("Sources"),
                        inspector: qsTr("Source"), mixer: qsTr("Audio mixer"), stats: qsTr("Stats") }
        for (const d of webDockList) names[d.id] = d.name
        return names
    }
    // The browser docks' pages share one profile. Made only while there are
    // docks: it starts the browser engine in this process.
    Loader { id: webProfile; objectName: "webDockProfile"; active: win.webDockList.length > 0; source: "qrc:/BrowserDockProfile.qml" }
    ListModel { id: webDockModel }
    // Brings the docks' list in line with the preferences, a row at a time,
    // so pages already open stay as they are.
    function syncWebDocks() {
        const want = backend.webDocksAvailable ? backend.browserDocks : []
        for (let i = webDockModel.count - 1; i >= 0; --i) {
            const row = webDockModel.get(i)
            if (want.some(d => d.id === row.dock)) continue
            const item = webDocks.itemAt(i)
            for (const column of [leftDocks, rightDocks])
                for (let n = 0; n < column.count; ++n) if (column.itemAt(n) === item) { column.takeItem(n); break }
            webDockModel.remove(i)
            laidOut = ""
        }
        for (const d of want) {
            let found = -1
            for (let i = 0; i < webDockModel.count; ++i) if (webDockModel.get(i).dock === d.id) found = i
            if (found < 0) webDockModel.append({ dock: d.id, name: d.name, url: d.url })
            else {
                if (webDockModel.get(found).name !== d.name) webDockModel.setProperty(found, "name", d.name)
                if (webDockModel.get(found).url !== d.url) webDockModel.setProperty(found, "url", d.url)
            }
        }
    }
    function webDockItem(id) {
        for (let i = 0; i < webDocks.count; ++i) if (webDocks.itemAt(i) && webDocks.itemAt(i).dockId === id) return webDocks.itemAt(i)
        return null
    }
    readonly property var defaultDocks: ({ left: ["studios", "transitions", "sources"], right: ["inspector", "mixer"] })
    function dockItem(id) { return { studios: studiosDock, transitions: transitionsDock, sources: sourcesDock, inspector: inspectorDock, mixer: mixerDock, stats: statsDock }[id] ?? webDockItem(id) }
    function dockSize(key, fallback) { const v = backend.dockSizes[key]; return v > 0 ? v : fallback }
    // Docks folded down to their headers, remembered app-wide.
    property var foldedDocks: ({})
    function dockFolded(id) { return foldedDocks[id] === true }
    function foldedOr(id, open) { return dockFolded(id) ? Theme.hNavHeader + Theme.hairline : open }
    function toggleDockFolded(id) {
        const next = Object.assign({}, foldedDocks)
        if (next[id]) delete next[id]; else next[id] = true
        foldedDocks = next
        backend.setPanelOpen("dock." + id + ".folded", next[id] === true)
    }
    // The open dock that takes a column's spare height: the last one open.
    function fillsColumn(id) {
        const d = docks(), column = d.left.includes(id) ? d.left : d.right
        const open = column.filter(x => !dockFolded(x))
        return open.length > 0 && open[open.length - 1] === id
    }
    // The saved layout, cleaned: known docks, each at most once.
    function docks() {
        const saved = backend.dockLayout
        if (!saved.left && !saved.right) return defaultDocks
        const seen = {}, out = { left: [], right: [] }, known = knownDocks()
        for (const side of ["left", "right"])
            for (const id of saved[side] ?? [])
                if (known.includes(id) && !seen[id]) { seen[id] = true; out[side].push(id) }
        // Layouts from before the mixer had its own dock: it goes under Source.
        if (!saved.mixerDock && !seen.mixer) {
            const side = out.left.includes("inspector") ? "left" : "right"
            const at = out[side].indexOf("inspector")
            out[side].splice(at < 0 ? out[side].length : at + 1, 0, "mixer")
        }
        // And from before transitions: under Studios.
        if (!saved.transitionsDock && !seen.transitions) {
            const side = out.right.includes("studios") ? "right" : "left"
            const at = out[side].indexOf("studios")
            out[side].splice(at < 0 ? 0 : at + 1, 0, "transitions")
        }
        // Stats starts closed, so a layout from before it gains nothing; the
        // flag says the layout knows of it, should it ever start open.
        return out
    }
    function dockSide(id) { const d = docks(); return d.left.includes(id) ? "left" : d.right.includes(id) ? "right" : "" }
    property string laidOut: ""
    function layoutDocks() {
        const d = docks(), key = JSON.stringify(d)
        if (key === laidOut) return
        laidOut = key
        for (const column of [leftDocks, rightDocks])
            while (column.count > 0) column.takeItem(0).parent = dockPool
        for (const id of d.left) leftDocks.addItem(dockItem(id))
        for (const id of d.right) rightDocks.addItem(dockItem(id))
    }
    // Puts a dock on a side at an index (-1 = the end), or closes it (side "").
    function placeDock(id, side, index) {
        const d = docks(), out = { left: d.left.filter(x => x !== id), right: d.right.filter(x => x !== id) }
        if (side !== "") {
            const at = index < 0 || index > out[side].length ? out[side].length : index
            out[side].splice(at, 0, id)
        }
        out.mixerDock = true   // knows about the mixer's own dock: a closed one stays closed
        out.transitionsDock = true
        out.statsDock = true
        backend.dockLayout = out
    }
    // The guide points at a panel: bring it out if it is hidden or folded.
    function revealForHelp(target) {
        const id = { studiosDock: "studios", transitionsDock: "transitions", sourcesDock: "sources",
                     inspectorDock: "inspector", mixerDock: "mixer", statsDock: "stats" }[target]
        if (id === undefined) return
        showDock(id)
        if (dockFolded(id)) toggleDockFolded(id)
    }
    function showDock(id) { if (dockSide(id) === "") placeDock(id, defaultDocks.left.includes(id) ? "left" : "right", -1) }
    function saveDockSizes() {
        const sizes = Object.assign({}, backend.dockSizes)
        if (leftDocks.visible) sizes.left = Math.round(leftDocks.width)
        if (rightDocks.visible) sizes.right = Math.round(rightDocks.width)
        // A folded dock keeps the height it had open.
        for (const column of [leftDocks, rightDocks])
            if (column.count > 1)
                for (let i = 0; i < column.count; ++i)
                    if (!dockFolded(column.itemAt(i).dockId)) sizes[column.itemAt(i).dockId] = Math.round(column.itemAt(i).height)
        if (JSON.stringify(sizes) !== JSON.stringify(backend.dockSizes)) backend.dockSizes = sizes
    }
    function resetDocks() {
        for (const id of Object.keys(foldedDocks)) backend.setPanelOpen("dock." + id + ".folded", false)
        foldedDocks = {}
        laidOut = ""; backend.dockSizes = {}; backend.dockLayout = {}
    }
    Component.onCompleted: {
        // Decided once, at the start, so ticking the option in Settings does not
        // take the window away: hidden only with a tray to bring it back from, and
        // never while a crash report waits for an answer.
        visible = backend.crashOffered || !(backend.startInTray && win.canHide)
        const folded = {}
        for (const id of dockIds) if (backend.panelOpen("dock." + id + ".folded", false)) folded[id] = true
        foldedDocks = folded
        syncWebDocks()
        layoutDocks()
    }
    Connections { target: backend; function onPreferencesChanged() { win.syncWebDocks(); win.layoutDocks() } }
    Connections { target: backend; function onStartStateChanged() { if (backend.crashOffered) win.showWindow() } }
    // The stats dock reads only while someone can see it.
    Binding {
        target: backend
        property: "statsWatched"
        value: win.visible && (win.laidOut, win.dockSide("stats") !== "") && !win.dockFolded("stats")
    }

    Menu {
        id: dockMenu
        objectName: "dockMenu"
        property string dockId: ""
        readonly property string side: dockId !== "" ? (win.laidOut, win.dockSide(dockId)) : ""
        readonly property var column: side !== "" ? win.docks()[side] : []
        readonly property int index: column.indexOf(dockId)
        function showFor(id, button) { dockId = id; popup(button, 0, button.height) }
        MenuItem { text: dockMenu.side === "left" ? qsTr("Move to the right side") : qsTr("Move to the left side")
                   enabled: !backend.docksLocked
                   onTriggered: win.placeDock(dockMenu.dockId, dockMenu.side === "left" ? "right" : "left", -1) }
        MenuItem { text: qsTr("Move up"); icon.name: "arrow-up"; enabled: !backend.docksLocked && dockMenu.index > 0
                   onTriggered: win.placeDock(dockMenu.dockId, dockMenu.side, dockMenu.index - 1) }
        MenuItem { text: qsTr("Move down"); icon.name: "arrow-down"; enabled: !backend.docksLocked && dockMenu.index < dockMenu.column.length - 1
                   onTriggered: win.placeDock(dockMenu.dockId, dockMenu.side, dockMenu.index + 1) }
        MenuSeparator {}
        MenuItem { text: qsTr("Close"); icon.name: "x"; enabled: !backend.docksLocked
                   onTriggered: win.placeDock(dockMenu.dockId, "", -1) }
        // A browser dock's own: its page, and the list of them.
        MenuSeparator { visible: dockMenu.dockId.startsWith("web-"); height: visible ? implicitHeight : 0 }
        MenuItem { objectName: "webDockReload"; text: qsTr("Reload page"); icon.name: "refresh-cw"
                   visible: dockMenu.dockId.startsWith("web-"); height: visible ? implicitHeight : 0
                   onTriggered: { const item = win.webDockItem(dockMenu.dockId); if (item) item.reload() } }
        MenuItem { objectName: "webDockEdit"; text: qsTr("Edit browser docks…"); icon.name: "pencil"
                   visible: dockMenu.dockId.startsWith("web-"); height: visible ? implicitHeight : 0
                   onTriggered: browserDocksSheet.open() }
    }
    BrowserDocksSheet { id: browserDocksSheet; onAdded: (id) => win.placeDock(id, "right", -1) }

    function renameScene(id) {
        const scene = backend.scenes.find(x => x.id === id)
        if (scene) nameSheet.ask(qsTr("Rename studio"), scene.name, (name) => backend.renameScene(id, name))
    }
    function renameSource(sourceId, name) {
        nameSheet.ask(qsTr("Rename source"), name, (n) => backend.renameSource(sourceId, n))
    }
    NameSheet { id: nameSheet }
    // Interact windows: one for each browser source asked for, raised when asked again.
    ListModel { id: interactSources }
    Connections {
        target: backend
        function onInteractRequested(sourceId) {
            const open = interactWindows.windowFor(sourceId)
            if (open) { open.raise(); open.requestActivate(); return }
            interactSources.append({ source: sourceId })
        }
    }
    Instantiator {
        id: interactWindows
        objectName: "interactWindows"
        model: interactSources
        function windowFor(sourceId) {
            for (let i = 0; i < count; ++i) if (objectAt(i) && objectAt(i).sourceId === sourceId) return objectAt(i)
            return null
        }
        delegate: InteractWindow {
            required property string source
            sourceId: source
            onDone: {
                for (let i = 0; i < interactSources.count; ++i)
                    if (interactSources.get(i).source === sourceId) { interactSources.remove(i); return }
            }
        }
    }
    // Projectors, reopened as they were left. A window is made again only when
    // a choice of its own changes (fullscreen, screen, on top), never another's;
    // its size, place and name it follows as it is.
    ListModel {
        id: projectorModel
        function key(p) { return JSON.stringify(Object.assign({}, p, { x: 0, y: 0, width: 0, height: 0, label: "" })) }
        function sync() {
            const wanted = backend.projectors
            for (let i = count - 1; i >= 0; --i) {
                const p = wanted.find(w => w.id === get(i).pid)
                if (!p || key(p) !== get(i).key) remove(i)
            }
            for (const p of wanted) {
                let have = false
                for (let j = 0; j < count && !have; ++j) have = get(j).pid === p.id
                if (!have) append({ pid: p.id, key: key(p) })
            }
        }
        Component.onCompleted: sync()
    }
    Connections { target: backend; function onProjectorsChanged() { projectorModel.sync() } }
    Instantiator {
        objectName: "projectors"
        model: projectorModel
        delegate: ProjectorWindow {
            required property string pid
            entry: backend.projectors.find(p => p.id === pid) ?? ({ id: pid })
        }
    }
    TransitionSheet { id: transitionSheet }
    SwitcherSheet { id: switcherSheet }
    OutputTimerSheet { id: outputTimerSheet }
    RemoteControlSheet { id: remoteControlSheet }
    SetupSheet { id: setupSheet }
    ScriptsSheet { id: scriptsSheet }
    StudioTransitionSheet { id: studioTransitionSheet }
    ItemMenu { id: itemMenu; onRenameRequested: (sourceId, name) => win.renameSource(sourceId, name) }
    SettingsSheet { id: settingsSheet }
    ProfilesSheet { id: profilesSheet }
    HelpSheet { id: helpSheet }
    HelpGuide {
        id: helpGuide
        onStartTask: (key) => helpCoach.start(helpGuide.topicOf(key))
        onShowPart: (target) => helpCoach.point(target)
        onKeysRequested: helpSheet.open()
    }
    Connections { target: Sheets; function onHelpRequested(topic) { helpGuide.showTopic(topic) } }
    HelpCoach {
        id: helpCoach
        scope: win.contentItem
        onTargetChanged: win.revealForHelp(target)
        onOpenGuide: (key) => helpGuide.showTopic(key)
    }
    LogSheet { id: logSheet }
    CrashSheet { id: crashSheet; onLogRequested: logSheet.showRun(true) }
    RepairSheet { id: repairSheet; onLayoutReset: win.resetDocks() }
    MissingFilesSheet { id: missingSheet }
    RemuxSheet { id: remuxSheet }
    SlideshowImagesSheet { id: slideshowSheet }
    VideoFiltersSheet { id: videoFiltersSheet }
    DirectCaptureSheet { id: directCaptureSheet }
    TransformSheet { id: transformSheet }
    Sheet {
        id: removeCollectionSheet
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s5 * 2, 420)
        modal: true
        title: qsTr("Remove this collection?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        contentItem: Label {
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            // Filled in on opening: a binding here loops through the sheet's size.
            text: ""
        }
        onAboutToShow: contentItem.text = qsTr("“%1”, its studios and sources go to the bin; the files they show stay where they are.")
                                              .arg(backend.collectionName)
        onAccepted: {
            const current = backend.collections.find(c => c.current)
            if (current) backend.removeCollection(current.file)
        }
    }

    // A window of its own while it is open.
    Loader {
        active: backend.prompterOpen
        sourceComponent: PrompterWindow {}
    }
    Loader {
        active: backend.playbackOpen
        sourceComponent: PlaybackWindow {}
    }
}

