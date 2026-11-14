import QtQuick
import OmaBroadcast 1.0

// The multiview: preview and program large with studios around them, or a
// grid of studios. A click previews a studio (studio mode) or makes it live;
// with Click takes on, a click in studio mode takes it too.
Item {
    id: multiview
    property string ownerId
    readonly property string layout: backend.multiviewLayout
    readonly property bool grid: layout.startsWith("grid")
    readonly property int across: grid ? Math.max(2, Math.round(Math.sqrt(parseInt(layout.substring(4)) || 4))) : (layout === "left" || layout === "right" ? 2 : 4)
    readonly property int studioCount: grid ? across * across : 8
    readonly property var studios: backend.scenes.slice(0, studioCount)

    // Where tile `i` goes: 0 and 1 are preview and program outside a grid.
    function place(i) {
        const W = width, H = height
        if (grid) {
            const w = W / across, h = H / across
            return Qt.rect((i % across) * w, Math.floor(i / across) * h, w, h)
        }
        const horizontal = layout === "top" || layout === "bottom"
        if (horizontal) {
            const bigTop = layout === "top", bigY = bigTop ? 0 : H / 2, smallY = bigTop ? H / 2 : 0
            if (i < 2) return Qt.rect(i * W / 2, bigY, W / 2, H / 2)
            const n = i - 2, w = W / 4, h = H / 4
            return Qt.rect((n % 4) * w, smallY + Math.floor(n / 4) * h, w, h)
        }
        const bigLeft = layout === "left", bigX = bigLeft ? 0 : W / 2, smallX = bigLeft ? W / 2 : 0
        if (i < 2) return Qt.rect(bigX, i * H / 2, W / 2, H / 2)
        const n = i - 2, w = W / 4, h = H / 4
        return Qt.rect(smallX + (n % 2) * w, Math.floor(n / 2) * h, w, h)
    }

    readonly property var tiles: (grid ? [] : [{ shows: "preview", target: "", label: qsTr("Preview"), fps: 30 },
                                                 { shows: "program", target: "", label: qsTr("Program"), fps: 30 }])
                                 .concat(studios.map(s => ({ shows: "studio", target: s.id, label: s.name, fps: 15 })))
    Repeater {
        model: multiview.tiles
        delegate: MultiviewTile {
            required property var modelData
            required property int index
            readonly property rect at: multiview.place(index)
            x: at.x; y: at.y; width: at.width; height: at.height
            objectName: "multiviewTile-" + index
            viewId: multiview.ownerId + "-" + index
            shows: modelData.shows
            target: modelData.target
            label: modelData.label
            fps: modelData.fps
            onPicked: {
                if (shows !== "studio") return
                backend.currentScene = target
                if (backend.studioMode && backend.multiviewClickTakes) backend.take()
            }
        }
    }
}
