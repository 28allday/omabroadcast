import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import OmaBroadcast 1.0
import "canvascursor.js" as Cursors
import "transform.js" as Geo

// Editing on the program preview: click to select, drag to move, handles to
// resize, the knob to rotate, Alt on a handle to crop. Everything is worked
// out in canvas pixels and drawn at the preview's scale.
Item {
    id: root

    property int canvasWidth: 1
    property int canvasHeight: 1
    // Locked: selecting and the menu still work, nothing moves.
    property bool locked: false
    // Dashed outlines for items reaching past the canvas edge.
    property bool showOverflow: false
    signal contextMenuRequested(string itemId, real x, real y)

    readonly property real s: width / Math.max(1, canvasWidth)
    readonly property var outlines: locked ? backend.outlines.map(o => Object.assign({}, o, { locked: true }))
                                           : backend.outlines
    // Top-level items with a corner off the canvas.
    readonly property var overflowing: {
        if (!showOverflow) return []
        return outlines.filter(o => !(o.depth > 0) && Geo.corners(o).some(c =>
            c.x < -0.5 || c.y < -0.5 || c.x > canvasWidth + 0.5 || c.y > canvasHeight + 0.5))
    }
    readonly property var sel: backend.selected
    readonly property bool hasSel: sel.id !== undefined
    // Handles for a single selection; outlines for every selected item.
    readonly property bool single: backend.selectedItems.length === 1
    readonly property var selOutline: {
        if (!single) return null
        for (const o of outlines) if (o.id === backend.selectedItem) return o
        return null
    }
    readonly property var selOutlines: outlines.filter(o => backend.selectedItems.indexOf(o.id) >= 0)
    // Guides shown while a move snaps: canvas x and y positions.
    property var guidesX: []
    property var guidesY: []
    property string hoverId: ""

    function canvasPoint(x, y) { return { x: x / s, y: y / s } }
    function others() { return outlines.filter(o => backend.selectedItems.indexOf(o.id) < 0 && !(o.depth > 0)) }

    focus: true
    Keys.onPressed: (event) => {
        if (backend.selectedItems.length === 0) return
        const step = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
        let dx = 0, dy = 0
        if (event.key === Qt.Key_Left) dx = -step
        else if (event.key === Qt.Key_Right) dx = step
        else if (event.key === Qt.Key_Up) dy = -step
        else if (event.key === Qt.Key_Down) dy = step
        else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            backend.removeSelectedItems()
            event.accepted = true
            return
        } else if (event.key === Qt.Key_Escape) {
            backend.selectedItem = ""
            event.accepted = true
            return
        } else return
        backend.setItemBoxes(root.selOutlines.filter(o => !o.locked)
                                 .map(o => ({ id: o.id, x: o.x + dx, y: o.y + dy, width: o.width, height: o.height })), true)
        event.accepted = true
    }
    Keys.onReleased: (event) => {
        if (!event.isAutoRepeat && [Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down].includes(event.key))
            backend.endGesture()
    }

    // ── Moving and selecting ────────────────────────────────────────────────
    MouseArea {
        id: area
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        hoverEnabled: true
        property var start: null      // outline when the drag began
        property var starts: []       // every selected outline when it began
        property point grab
        property bool moving: false
        cursorShape: moving ? Qt.ClosedHandCursor : root.hoverId !== "" ? Qt.OpenHandCursor : Qt.ArrowCursor

        onPositionChanged: (mouse) => {
            const p = root.canvasPoint(mouse.x, mouse.y)
            if (!pressed) {
                root.hoverId = Geo.hit(root.outlines, p.x, p.y)
                return
            }
            if (!moving || !start) return
            let nx = start.x + (p.x - grab.x), ny = start.y + (p.y - grab.y)
            // The whole selection moves with the one grabbed, by the same amount.
            root.guidesX = []
            root.guidesY = []
            if (!(mouse.modifiers & Qt.ControlModifier)) {
                const moved = { x: nx, y: ny, width: start.width, height: start.height, rotation: start.rotation }
                const b = Geo.bounds(moved)
                const snap = Geo.snapMove({ x: b.x, y: b.y, width: b.x2 - b.x, height: b.y2 - b.y },
                                          root.others(), root.canvasWidth, root.canvasHeight,
                                          Theme.snapPixels / root.s)
                nx += snap.dx
                ny += snap.dy
                root.guidesX = snap.guidesX
                root.guidesY = snap.guidesY
            }
            const dx = nx - start.x, dy = ny - start.y
            backend.setItemBoxes(starts.map(o => ({ id: o.id, x: o.x + dx, y: o.y + dy, width: o.width, height: o.height })), true)
        }
        onPressed: (mouse) => {
            root.forceActiveFocus()
            const p = root.canvasPoint(mouse.x, mouse.y)
            // Pressing a selected item keeps the selection (it can be dragged
            // together); Shift adds or removes; elsewhere selects what is hit.
            const onSelected = root.selOutlines.find(o => !o.locked && Geo.contains(o, p.x, p.y))
            // A locked preview still selects: hit-test the items' own locks.
            const id = onSelected ? onSelected.id : Geo.hit(backend.outlines, p.x, p.y)
            if (mouse.modifiers & Qt.ShiftModifier) {
                if (id === "") return
                const sel = backend.selectedItems.slice()
                const at = sel.indexOf(id)
                if (at >= 0) sel.splice(at, 1); else sel.push(id)
                backend.selectedItems = sel
                return
            }
            if (!onSelected) backend.selectedItem = id
            if (mouse.button === Qt.RightButton) {
                if (id !== "") root.contextMenuRequested(id, mouse.x, mouse.y)
                return
            }
            const o = root.outlines.find(x => x.id === id)
            if (!o || o.locked) return
            start = o
            starts = root.outlines.filter(x => backend.selectedItems.indexOf(x.id) >= 0 && !x.locked)
            grab = Qt.point(p.x, p.y)
            moving = true
        }
        onReleased: {
            if (moving) backend.endGesture()
            moving = false
            start = null
            root.guidesX = []
            root.guidesY = []
        }
        onExited: root.hoverId = ""
    }

    // Every selected item's outline (the handles come with a single one).
    Repeater {
        model: root.single ? [] : root.selOutlines
        Rectangle {
            required property var modelData
            x: modelData.x * root.s
            y: modelData.y * root.s
            width: modelData.width * root.s
            height: modelData.height * root.s
            rotation: modelData.rotation
            color: "transparent"
            border.width: Theme.selectionRing
            border.color: Theme.selectionOutline
        }
    }

    // Hover outline: what a click would pick.
    Repeater {
        model: root.outlines
        Rectangle {
            required property var modelData
            visible: modelData.id === root.hoverId && modelData.id !== backend.selectedItem
            x: modelData.x * root.s
            y: modelData.y * root.s
            width: modelData.width * root.s
            height: modelData.height * root.s
            rotation: modelData.rotation
            color: "transparent"
            border.width: Theme.hairline
            border.color: Theme.hoverOutline
        }
    }

    Repeater {
        model: root.overflowing
        Shape {
            required property var modelData
            objectName: "overflowOutline"
            readonly property var c: Geo.corners(modelData)
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: Theme.hoverOutline
                strokeWidth: Theme.hairline
                strokeStyle: ShapePath.DashLine
                dashPattern: [4, 3]
                fillColor: "transparent"
                startX: c[0].x * root.s; startY: c[0].y * root.s
                PathLine { x: c[1].x * root.s; y: c[1].y * root.s }
                PathLine { x: c[2].x * root.s; y: c[2].y * root.s }
                PathLine { x: c[3].x * root.s; y: c[3].y * root.s }
                PathLine { x: c[0].x * root.s; y: c[0].y * root.s }
            }
        }
    }

    // Snap guides.
    Repeater {
        model: root.guidesX
        Rectangle {
            required property real modelData
            x: modelData * root.s
            y: 0
            width: Theme.hairline
            height: root.height
            color: Theme.snapGuide
        }
    }
    Repeater {
        model: root.guidesY
        Rectangle {
            required property real modelData
            x: 0
            y: modelData * root.s
            width: root.width
            height: Theme.hairline
            color: Theme.snapGuide
        }
    }

    // ── The selection ───────────────────────────────────────────────────────
    Item {
        id: box
        visible: root.selOutline !== null
        readonly property var o: root.selOutline ?? { x: 0, y: 0, width: 0, height: 0, rotation: 0, locked: true }
        x: o.x * root.s
        y: o.y * root.s
        width: o.width * root.s
        height: o.height * root.s
        rotation: o.rotation

        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.color: Theme.selectionOutline
            border.width: Theme.selectionRing
        }

        // The rotation knob on its stalk.
        Rectangle {
            visible: !box.o.locked
            width: Theme.hairline; height: Theme.s5
            anchors.horizontalCenter: parent.horizontalCenter; y: -height
            color: Theme.accent
        }
        Rectangle {
            visible: !box.o.locked
            width: Theme.szHandle * 1.5; height: width; radius: width / 2
            color: Theme.handleBg; border.color: Theme.accent; border.width: Theme.selectionRing
            anchors.horizontalCenter: parent.horizontalCenter; y: -Theme.s5 - height
            MouseArea {
                anchors.fill: parent; anchors.margins: -Theme.szHandleHit
                preventStealing: true
                cursorShape: Qt.CrossCursor
                property point pivot
                property real lastAngle
                property real startRotation
                property real total
                function angle(mouse) {
                    const g = mapToItem(root, mouse.x, mouse.y)
                    const p = root.canvasPoint(g.x, g.y)
                    return Math.atan2(p.y - pivot.y, p.x - pivot.x) * 180 / Math.PI
                }
                onPressed: (mouse) => {
                    const o = box.o
                    pivot = Qt.point(root.sel.pivotX ?? (o.x + o.width / 2), root.sel.pivotY ?? (o.y + o.height / 2))
                    lastAngle = angle(mouse)
                    startRotation = root.sel.rotation
                    total = 0
                }
                onPositionChanged: (mouse) => {
                    if (!pressed) return
                    const next = angle(mouse)
                    let d = next - lastAngle
                    if (d > 180) d -= 360
                    if (d < -180) d += 360
                    total += d
                    lastAngle = next
                    let r = startRotation + total
                    if (mouse.modifiers & Qt.ShiftModifier) r = Math.round(r / Theme.rotateSnapDegrees) * Theme.rotateSnapDegrees
                    backend.setItemRotation(backend.selectedItem, r, true)
                }
                onReleased: backend.endGesture()
            }
        }

        // Eight handles: resize from a corner or an edge; with Alt, crop.
        Repeater {
            model: [
                { hx: 0, hy: 0 }, { hx: 0.5, hy: 0 }, { hx: 1, hy: 0 }, { hx: 1, hy: 0.5 },
                { hx: 1, hy: 1 }, { hx: 0.5, hy: 1 }, { hx: 0, hy: 1 }, { hx: 0, hy: 0.5 }
            ]
            Rectangle {
                required property var modelData
                readonly property int cursor: [Qt.SizeHorCursor, Qt.SizeFDiagCursor, Qt.SizeVerCursor, Qt.SizeBDiagCursor][Cursors.handleAxis(modelData.hx, modelData.hy, box.rotation)]
                visible: !box.o.locked
                width: Cursors.handleSize(Theme.szHandle)
                height: width
                radius: Theme.rHandle
                color: handle.cropping ? Theme.accent : Theme.handleBg
                border.color: Theme.accent
                border.width: Theme.selectionRing
                x: parent.width * modelData.hx - width / 2
                y: parent.height * modelData.hy - height / 2

                MouseArea {
                    id: handle
                    anchors.fill: parent
                    anchors.margins: -Theme.szHandleHit
                    preventStealing: true
                    cursorShape: parent.cursor
                    property point grab
                    property var start
                    property var startSel
                    property bool cropping: false
                    onPressed: (mouse) => {
                        const g = mapToItem(root, mouse.x, mouse.y)
                        const p = root.canvasPoint(g.x, g.y)
                        grab = Qt.point(p.x, p.y)
                        start = Object.assign({}, box.o)
                        startSel = Object.assign({}, root.sel)
                        cropping = (mouse.modifiers & Qt.AltModifier) !== 0
                    }
                    onPositionChanged: (mouse) => {
                        if (!pressed) return
                        const g = mapToItem(root, mouse.x, mouse.y)
                        const p = root.canvasPoint(g.x, g.y)
                        const dx = p.x - grab.x, dy = p.y - grab.y
                        const hx = parent.modelData.hx, hy = parent.modelData.hy
                        if (cropping) {
                            root.cropFromHandle(start, startSel, hx, hy, dx, dy)
                            return
                        }
                        const r = Geo.resize(start, hx, hy, dx, dy, (mouse.modifiers & Qt.ShiftModifier) !== 0)
                        backend.setItemBox(backend.selectedItem, r.x, r.y, r.width, r.height, true)
                    }
                    onReleased: { cropping = false; backend.endGesture() }
                }
            }
        }
    }

    // Alt on a handle: that edge cuts into the source. An unbounded item keeps
    // its scale, so the picture stays put and the outline shrinks to the cut;
    // a bounded one re-fits the cropped picture in its box.
    function cropFromHandle(o, s, hx, hy, dx, dy) {
        const c = Math.cos(Geo.rad(o.rotation)), sn = Math.sin(Geo.rad(o.rotation))
        const lx = c * dx + sn * dy, ly = -sn * dx + c * dy
        const sw = s.sourceWidth, sh = s.sourceHeight
        const kx = (sw - s.cropLeft - s.cropRight) / o.width
        const ky = (sh - s.cropTop - s.cropBottom) / o.height
        let l = s.cropLeft, t = s.cropTop, r = s.cropRight, b = s.cropBottom
        // Crops count in the source's pixels and a mirror comes after them, so on a
        // mirrored item the left handle cuts the source's right edge.
        const flipH = s.flipH === true, flipV = s.flipV === true
        if (hx === 0) { if (flipH) r = Math.round(s.cropRight + lx * kx); else l = Math.round(s.cropLeft + lx * kx) }
        if (hx === 1) { if (flipH) l = Math.round(s.cropLeft - lx * kx); else r = Math.round(s.cropRight - lx * kx) }
        if (hy === 0) { if (flipV) b = Math.round(s.cropBottom + ly * ky); else t = Math.round(s.cropTop + ly * ky) }
        if (hy === 1) { if (flipV) t = Math.round(s.cropTop - ly * ky); else b = Math.round(s.cropBottom - ly * ky) }
        l = Math.max(0, l); r = Math.max(0, r); t = Math.max(0, t); b = Math.max(0, b)
        if (s.bounds !== "none") {
            backend.setItemCrop(backend.selectedItem, l, t, r, b, true)
            return
        }
        const w = Math.max(1, (sw - l - r) / kx), h = Math.max(1, (sh - t - b) / ky)
        const placed = Geo.place(o, 1 - hx, 1 - hy, w, h)
        backend.setItemCropAt(backend.selectedItem, l, t, r, b, placed.x, placed.y, true)
    }
}
