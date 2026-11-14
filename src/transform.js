.pragma library
// Canvas-editing geometry for the preview's handles: pure functions on
// outlines {x, y, width, height, rotation} in canvas pixels, rotation in
// degrees clockwise about the outline's centre — as the engine draws them.

function rad(deg) { return deg * Math.PI / 180 }

// A canvas point in the outline's own frame (0..width, 0..height inside).
function toLocal(o, px, py) {
    const cx = o.x + o.width / 2, cy = o.y + o.height / 2
    const c = Math.cos(rad(o.rotation)), s = Math.sin(rad(o.rotation))
    const dx = px - cx, dy = py - cy
    return { x: c * dx + s * dy + o.width / 2, y: -s * dx + c * dy + o.height / 2 }
}

// The outline's point at (u, v) in 0..1 of its own frame, on the canvas.
function toCanvas(o, u, v) {
    const c = Math.cos(rad(o.rotation)), s = Math.sin(rad(o.rotation))
    const lx = (u - 0.5) * o.width, ly = (v - 0.5) * o.height
    return { x: o.x + o.width / 2 + c * lx - s * ly, y: o.y + o.height / 2 + s * lx + c * ly }
}

function contains(o, px, py) {
    const p = toLocal(o, px, py)
    return p.x >= 0 && p.y >= 0 && p.x <= o.width && p.y <= o.height
}

// The topmost unlocked top-level outline under the point, or "". (Items
// inside groups are picked from the list; a click takes the group.)
function hit(outlines, px, py) {
    for (let i = outlines.length - 1; i >= 0; --i)
        if (!outlines[i].locked && !(outlines[i].depth > 0) && contains(outlines[i], px, py)) return outlines[i].id
    return ""
}

// Resizing from handle (hx, hy) by a canvas-space drag (dx, dy): the opposite
// point stays where it is, rotated or not. Corners keep the aspect unless
// `free`. Returns the new {x, y, width, height}.
function resize(o, hx, hy, dx, dy, free) {
    const c = Math.cos(rad(o.rotation)), s = Math.sin(rad(o.rotation))
    const lx = c * dx + s * dy, ly = -s * dx + c * dy
    const sx = hx === 1 ? 1 : hx === 0 ? -1 : 0
    const sy = hy === 1 ? 1 : hy === 0 ? -1 : 0
    let w = Math.max(1, o.width + lx * sx), h = Math.max(1, o.height + ly * sy)
    if (!free && sx !== 0 && sy !== 0) {
        const k = Math.max(w / o.width, h / o.height)
        w = Math.max(1, o.width * k)
        h = Math.max(1, o.height * k)
    }
    return place(o, 1 - hx, 1 - hy, w, h)
}

// A box of the new size whose (u, v) point lands where the old one's did.
function place(o, u, v, w, h) {
    const anchor = toCanvas(o, u, v)
    const c = Math.cos(rad(o.rotation)), s = Math.sin(rad(o.rotation))
    const lx = (u - 0.5) * w, ly = (v - 0.5) * h
    const cx = anchor.x - (c * lx - s * ly), cy = anchor.y - (s * lx + c * ly)
    return { x: cx - w / 2, y: cy - h / 2, width: w, height: h }
}

// The outline's four corners on the canvas, clockwise from its top-left.
function corners(o) {
    return [toCanvas(o, 0, 0), toCanvas(o, 1, 0), toCanvas(o, 1, 1), toCanvas(o, 0, 1)]
}

// The axis-aligned box around an outline.
function bounds(o) {
    const pts = [toCanvas(o, 0, 0), toCanvas(o, 1, 0), toCanvas(o, 0, 1), toCanvas(o, 1, 1)]
    const xs = pts.map(p => p.x), ys = pts.map(p => p.y)
    return { x: Math.min(...xs), y: Math.min(...ys), x2: Math.max(...xs), y2: Math.max(...ys) }
}

// Snaps a moving box's edges and centre to the canvas and to other outlines,
// within `tolerance` canvas pixels. Returns {dx, dy, guidesX: [], guidesY: []}:
// the shift to apply and the lines to draw.
function snapMove(box, others, canvasW, canvasH, tolerance) {
    const xs = [0, canvasW / 2, canvasW], ys = [0, canvasH / 2, canvasH]
    for (const o of others) {
        const b = bounds(o)
        xs.push(b.x, (b.x + b.x2) / 2, b.x2)
        ys.push(b.y, (b.y + b.y2) / 2, b.y2)
    }
    function best(edges, targets) {
        let pick = null
        for (const e of edges)
            for (const t of targets) {
                const d = t - e
                if (Math.abs(d) <= tolerance && (pick === null || Math.abs(d) < Math.abs(pick.d))) pick = { d: d, at: t }
            }
        return pick
    }
    const sx = best([box.x, box.x + box.width / 2, box.x + box.width], xs)
    const sy = best([box.y, box.y + box.height / 2, box.y + box.height], ys)
    return { dx: sx ? sx.d : 0, dy: sy ? sy.d : 0, guidesX: sx ? [sx.at] : [], guidesY: sy ? [sy.at] : [] }
}
