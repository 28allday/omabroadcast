import QtQuick
import QtQuick.Controls
import QtQuick.Window
import OmaBroadcast 1.0

// A browser source's page in a window of its own, to use by hand: clicks,
// scrolling and typing go to the page (to sign in, press a button, dismiss
// a notice) and what it shows is what goes out.
ApplicationWindow {
    id: interact
    objectName: "interactWindow"
    property string sourceId: ""
    // Its name and how it is doing; empty once the source is gone.
    property var row: backend.browserStatus(sourceId)
    function refresh() { row = backend.browserStatus(sourceId); if (row.name === undefined && sourceId !== "") interact.done() }
    Connections { target: backend; function onItemsChanged() { interact.refresh() } }
    signal done()
    title: qsTr("Interact — %1").arg(row.name ?? "")
    color: Theme.windowBg
    // The page at its own size where the screen has room.
    width: Math.min(view.imageWidth > 0 ? view.imageWidth : 800, (screen ? screen.desktopAvailableWidth : 1280) * 0.8)
    height: Math.min(view.imageHeight > 0 ? view.imageHeight : 600, (screen ? screen.desktopAvailableHeight : 800) * 0.8)
    visible: true

    Component.onCompleted: backend.watchBrowser(sourceId, true)
    onClosing: interact.done()
    Component.onDestruction: backend.watchBrowser(sourceId, false)
    // The page has the keyboard while this window does.
    onActiveChanged: backend.browserInput(sourceId, { t: "focus", on: active })
    // Gone from the collection: nothing left to use.

    Connections {
        target: backend
        function onBrowserFrame(id, image) { if (id === interact.sourceId) view.setImage(image) }
    }

    // Transparent parts of the page show as a checkerboard.
    Canvas {
        anchors.fill: view
        onPaint: {
            const g = getContext("2d"), s = 12
            for (let y = 0; y < height; y += s)
                for (let x = 0; x < width; x += s) {
                    g.fillStyle = ((x + y) / s) % 2 === 0 ? "#3a3a3a" : "#2a2a2a"
                    g.fillRect(x, y, s, s)
                }
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }
    PreviewView {
        id: view
        objectName: "interactView"
        anchors.centerIn: parent
        readonly property real aspect: imageHeight > 0 ? imageWidth / imageHeight : 4 / 3
        width: Math.min(parent.width, parent.height * aspect)
        height: width / aspect
    }
    Label {
        anchors.centerIn: parent
        visible: view.imageWidth === 0
        text: qsTr("Waiting for the page… (%1)").arg(interact.row.state ?? "")
        color: Theme.textMuted
    }

    // Window points to the page's own pixels.
    function pageX(x) { return Math.round(x * view.imageWidth / Math.max(1, view.width)) }
    function pageY(y) { return Math.round(y * view.imageHeight / Math.max(1, view.height)) }
    function send(event) { backend.browserInput(interact.sourceId, event) }

    MouseArea {
        id: pointer
        objectName: "interactPointer"
        anchors.fill: view
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        focus: true
        function mouse(type, m) {
            interact.send({ t: "mouse", type: type, x: interact.pageX(m.x), y: interact.pageY(m.y),
                            button: type === "move" ? 0 : m.button, buttons: m.buttons, modifiers: m.modifiers })
        }
        onPressed: (m) => { forceActiveFocus(); mouse("press", m) }
        onReleased: (m) => mouse("release", m)
        onPositionChanged: (m) => mouse("move", m)
        onDoubleClicked: (m) => mouse("double", m)
        onWheel: (w) => interact.send({ t: "wheel", x: interact.pageX(w.x), y: interact.pageY(w.y),
                                        dx: w.angleDelta.x, dy: w.angleDelta.y, modifiers: w.modifiers })
        // Every key goes to the page, the app's own shortcuts included.
        Keys.onPressed: (k) => {
            interact.send({ t: "key", type: "press", key: k.key, modifiers: k.modifiers, text: k.text, repeat: k.isAutoRepeat })
            k.accepted = true
        }
        Keys.onReleased: (k) => {
            interact.send({ t: "key", type: "release", key: k.key, modifiers: k.modifiers, text: k.text, repeat: k.isAutoRepeat })
            k.accepted = true
        }
        Keys.onShortcutOverride: (k) => { k.accepted = true }
    }
}
