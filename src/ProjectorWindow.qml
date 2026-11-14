import QtQuick
import QtQuick.Controls
import QtQuick.Window
import OmaBroadcast 1.0

// A projector: the program, the preview, a studio, a source or the multiview
// in a window of its own, windowed or filling a screen. Right-click for its
// options; Escape leaves fullscreen, then closes it.
ApplicationWindow {
    id: projector
    property var entry: ({})
    readonly property string viewId: entry.id ?? ""
    readonly property bool onTop: entry.onTop === true && entry.fullscreen !== true
    // Not translated: Hyprland's on-top rule finds the window by this title.
    title: (onTop ? "OmaBroadcast projector (on top) — " : "OmaBroadcast projector — ") + (entry.label ?? "")
    color: "black"
    width: entry.width > 0 ? entry.width : 960
    height: entry.height > 0 ? entry.height : 540
    flags: onTop ? Qt.Window | Qt.WindowStaysOnTopHint : Qt.Window

    Component.onCompleted: {
        const screens = Qt.application.screens
        if (entry.fullscreen === true && entry.screen >= 0 && entry.screen < screens.length) screen = screens[entry.screen]
        visibility = entry.fullscreen === true ? Window.FullScreen : Window.Windowed
        settle.restart()
    }
    onClosing: backend.closeProjector(viewId)
    Component.onDestruction: if (entry.shows !== "multiview") backend.releaseView(viewId)

    // Remembered size, for the next run.
    onWidthChanged: { settle.restart(); if (entry.fullscreen !== true) remember.restart() }
    onHeightChanged: { settle.restart(); if (entry.fullscreen !== true) remember.restart() }
    Timer { id: remember; interval: 500
            onTriggered: { backend.setProjectorOption(projector.viewId, "width", projector.width); backend.setProjectorOption(projector.viewId, "height", projector.height) } }
    Timer { id: settle; interval: 100; onTriggered: if (projector.entry.shows !== "multiview")
            backend.requestView(projector.viewId, projector.entry.shows, projector.entry.target ?? "",
                                Math.round(projector.width * projector.devicePixelRatio), Math.round(projector.height * projector.devicePixelRatio), 30) }

    PreviewView {
        id: view
        objectName: "projectorView"
        visible: projector.entry.shows !== "multiview"
        anchors.centerIn: parent
        readonly property real aspect: imageHeight > 0 ? imageWidth / imageHeight : 16 / 9
        width: Math.min(parent.width, parent.height * aspect)
        height: width / aspect
    }
    Loader {
        anchors.fill: parent
        active: projector.entry.shows === "multiview"
        sourceComponent: MultiviewContent { ownerId: projector.viewId }
    }
    Connections {
        target: backend
        function onViewReady(id, image) { if (id === projector.viewId) view.setImage(image) }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        // Clicks pass to the multiview's tiles; the cursor hides when asked.
        propagateComposedEvents: true
        cursorShape: projector.entry.hideCursor === true ? Qt.BlankCursor : Qt.ArrowCursor
        onClicked: (mouse) => menu.popup()
    }
    Shortcut {
        sequence: "Escape"
        onActivated: projector.entry.fullscreen === true ? backend.setProjectorOption(projector.viewId, "fullscreen", false) : projector.close()
    }

    Menu {
        id: menu
        objectName: "projectorMenu"
        Menu {
            id: screenMenu
            title: qsTr("Fullscreen on")
            Instantiator {
                model: backend.screens
                delegate: MenuItem {
                    required property var modelData
                    text: qsTr("%1 (%2 × %3)").arg(modelData.name).arg(modelData.width).arg(modelData.height)
                    checkable: true
                    checked: projector.entry.fullscreen === true && projector.entry.screen === modelData.index
                    onTriggered: { backend.setProjectorOption(projector.viewId, "screen", modelData.index); backend.setProjectorOption(projector.viewId, "fullscreen", true) }
                }
                onObjectAdded: (index, object) => screenMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => screenMenu.removeItem(object)
            }
        }
        MenuItem { text: qsTr("Windowed"); enabled: projector.entry.fullscreen === true
                   onTriggered: backend.setProjectorOption(projector.viewId, "fullscreen", false) }
        MenuItem { text: qsTr("Always on top"); checkable: true; checked: projector.entry.onTop === true; enabled: projector.entry.fullscreen !== true
                   onTriggered: backend.setProjectorOption(projector.viewId, "onTop", !(projector.entry.onTop === true)) }
        MenuItem { text: qsTr("Hide the cursor"); checkable: true; checked: projector.entry.hideCursor === true
                   onTriggered: backend.setProjectorOption(projector.viewId, "hideCursor", !(projector.entry.hideCursor === true)) }
        MenuSeparator { visible: projector.entry.shows === "multiview" }
        Menu {
            id: layoutMenu
            title: qsTr("Layout")
            enabled: projector.entry.shows === "multiview"
            Instantiator {
                model: [{ id: "top", name: qsTr("Preview and program above") }, { id: "bottom", name: qsTr("Preview and program below") },
                        { id: "left", name: qsTr("Preview and program left") }, { id: "right", name: qsTr("Preview and program right") },
                        { id: "grid4", name: qsTr("4 studios") }, { id: "grid9", name: qsTr("9 studios") },
                        { id: "grid16", name: qsTr("16 studios") }, { id: "grid25", name: qsTr("25 studios") }]
                delegate: MenuItem {
                    required property var modelData
                    text: modelData.name
                    checkable: true
                    checked: backend.multiviewLayout === modelData.id
                    onTriggered: backend.multiviewLayout = modelData.id
                }
                onObjectAdded: (index, object) => layoutMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => layoutMenu.removeItem(object)
            }
        }
        MenuItem { visible: projector.entry.shows === "multiview"; height: visible ? implicitHeight : 0
                   text: qsTr("Studio names"); checkable: true; checked: backend.multiviewLabels
                   onTriggered: backend.multiviewLabels = !backend.multiviewLabels }
        MenuItem { visible: projector.entry.shows === "multiview"; height: visible ? implicitHeight : 0
                   text: qsTr("Safe areas"); checkable: true; checked: backend.multiviewSafeAreas
                   onTriggered: backend.multiviewSafeAreas = !backend.multiviewSafeAreas }
        MenuItem { visible: projector.entry.shows === "multiview"; height: visible ? implicitHeight : 0
                   text: qsTr("A click takes (studio mode)"); checkable: true; checked: backend.multiviewClickTakes
                   onTriggered: backend.multiviewClickTakes = !backend.multiviewClickTakes }
        MenuSeparator {}
        MenuItem { text: qsTr("Close projector"); icon.name: "x"; onTriggered: projector.close() }
    }
}
