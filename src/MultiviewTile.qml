import QtQuick
import QtQuick.Controls
import QtQuick.Window
import OmaBroadcast 1.0

// One picture in the multiview: the preview, the program or a studio, named,
// marked when it is on the program or being previewed, with safe-area guides.
Rectangle {
    id: tile
    property string viewId
    property string shows: "studio"
    property string target: ""
    property string label: ""
    property int fps: 15
    readonly property bool onProgram: shows === "program" || (shows === "studio" && target === backend.programScene)
    readonly property bool previewed: shows === "preview" || (backend.studioMode && shows === "studio" && target === backend.currentScene)
    signal picked()
    color: "black"
    border.width: onProgram || previewed ? Math.max(2, Math.round(height / 90)) : 1
    border.color: onProgram ? Theme.recording : previewed ? Theme.accent : Theme.border

    readonly property real aspect: backend.canvasHeight > 0 ? backend.canvasWidth / backend.canvasHeight : 16 / 9
    readonly property real roomW: width - border.width * 2
    readonly property real roomH: height - border.width * 2
    Item {
        id: picture
        anchors.centerIn: parent
        width: Math.min(tile.roomW, tile.roomH * tile.aspect)
        height: width / tile.aspect
        PreviewView { id: view; anchors.fill: parent }
        // Action-safe (93 %) and title-safe (90 %) guides.
        Repeater {
            model: backend.multiviewSafeAreas ? [0.035, 0.05] : []
            delegate: Rectangle {
                required property real modelData
                x: picture.width * modelData; y: picture.height * modelData
                width: picture.width * (1 - modelData * 2); height: picture.height * (1 - modelData * 2)
                color: "transparent"
                border.width: 1
                border.color: Qt.rgba(1, 1, 1, 0.5)
            }
        }
    }
    Rectangle {
        visible: backend.multiviewLabels && tile.label !== ""
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: tile.border.width + Theme.s1
        width: name.implicitWidth + Theme.s3 * 2
        height: name.implicitHeight + Theme.s1 * 2
        radius: Theme.rControl
        color: Qt.rgba(0, 0, 0, 0.6)
        Label { id: name; anchors.centerIn: parent; text: tile.label; color: "white"; font.pixelSize: Math.max(10, Math.min(18, tile.height / 14)) }
    }
    TapHandler { onTapped: tile.picked() }

    function ask() {
        if (tile.viewId === "" || width < 4 || height < 4) return
        const dpr = Window.window ? Window.window.devicePixelRatio : 1
        backend.requestView(tile.viewId, tile.shows, tile.target, Math.round(picture.width * dpr), Math.round(picture.height * dpr), tile.fps)
    }
    Timer { id: settle; interval: 100; onTriggered: tile.ask() }
    onWidthChanged: settle.restart()
    onHeightChanged: settle.restart()
    onShowsChanged: settle.restart()
    onTargetChanged: settle.restart()
    Component.onCompleted: settle.restart()
    Component.onDestruction: if (tile.viewId !== "") backend.releaseView(tile.viewId)
    Connections {
        target: backend
        function onViewReady(id, image) { if (id === tile.viewId) view.setImage(image) }
    }
}
