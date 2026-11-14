import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// One grading wheel: its point pushes a tonal range towards a colour (the
// colours sit where the vectorscope shows them), its level below. Drag the
// point from wherever it is (Shift for fine); double-click puts it back in
// the middle. The sheet owns the values: this only asks for changes, `drag`
// true while a drag goes on, then `finished`.
ColumnLayout {
    id: wheel
    spacing: Theme.s1
    property string label: ""
    property string tip: ""
    property string name: ""            // in object names: colourWheel_lift…
    property real pointX: 0
    property real pointY: 0
    property real level: 0
    property real initialLevel: 0
    property real minimum: -1
    property real maximum: 1
    signal pointMoved(real x, real y, bool drag)
    signal levelMoved(real value, bool drag)
    signal finished()
    signal reset()                      // point to the middle, level as it starts: one change

    RowLayout {
        Layout.fillWidth: true
        Label {
            Layout.fillWidth: true
            text: wheel.label
            color: Theme.textSecondary
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }
        ToolButton {
            objectName: "colourWheelReset_" + wheel.name
            icon.name: "rotate-ccw"
            enabled: wheel.pointX !== 0 || wheel.pointY !== 0 || wheel.level !== wheel.initialLevel
            onClicked: wheel.reset()
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("Put %1 back as it was").arg(wheel.label)
            Accessible.name: ToolTip.text
        }
    }
    Item {
        id: disc
        objectName: "colourWheel_" + wheel.name
        Layout.fillWidth: true
        Layout.preferredHeight: width
        Layout.minimumWidth: Theme.s6 * 2
        readonly property real radius: Math.min(width, height) / 2 - Theme.s1
        readonly property point centre: Qt.point(width / 2, height / 2)
        ToolTip.visible: area.containsMouse && !area.pressed; ToolTip.delay: Theme.tooltipDelay
        ToolTip.text: wheel.tip
        Accessible.role: Accessible.Dial
        Accessible.name: wheel.label

        // The hue ring, faded to grey in the middle: Cb along x, Cr up, as engine filterParams reads it.
        Canvas {
            anchors.fill: parent
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                const g = getContext("2d"), c = disc.centre, r = disc.radius, n = 180
                g.reset()
                for (let i = 0; i < n; ++i) {
                    const a0 = i / n * 2 * Math.PI, a1 = (i + 1.5) / n * 2 * Math.PI
                    const mid = (a0 + a1) / 2, cb = Math.cos(mid) * 0.5, cr = Math.sin(mid) * 0.5
                    let rr = 0.5 + 1.5748 * cr, bb = 0.5 + 1.8556 * cb, gg = (0.5 - 0.2126 * rr - 0.0722 * bb) / 0.7152
                    const top = Math.max(rr, gg, bb), lo = Math.min(rr, gg, bb, 0)
                    rr = (rr - lo) / (top - lo); gg = (gg - lo) / (top - lo); bb = (bb - lo) / (top - lo)
                    g.beginPath()
                    g.moveTo(c.x, c.y)
                    // Canvas angles turn clockwise: Cr up is the negative angle.
                    g.arc(c.x, c.y, r, -a0, -a1, true)
                    g.closePath()
                    // Muted, as the picture's colours look beside a dark window.
                    g.fillStyle = Qt.rgba(0.18 + rr * 0.5, 0.18 + gg * 0.5, 0.18 + bb * 0.5, 1)
                    g.fill()
                }
                const grey = g.createRadialGradient(c.x, c.y, 0, c.x, c.y, r)
                grey.addColorStop(0, Qt.rgba(0.26, 0.26, 0.27, 1))
                grey.addColorStop(0.7, Qt.rgba(0.26, 0.26, 0.27, 0.6))
                grey.addColorStop(1, Qt.rgba(0.26, 0.26, 0.27, 0.1))
                g.beginPath(); g.arc(c.x, c.y, r, 0, 2 * Math.PI); g.fillStyle = grey; g.fill()
                g.strokeStyle = Theme.border; g.lineWidth = 1
                g.beginPath(); g.arc(c.x, c.y, r, 0, 2 * Math.PI); g.stroke()
                g.beginPath(); g.moveTo(c.x - 6, c.y); g.lineTo(c.x + 6, c.y); g.moveTo(c.x, c.y - 6); g.lineTo(c.x, c.y + 6)
                g.strokeStyle = Qt.rgba(0.85, 0.85, 0.85, 0.6); g.stroke()
            }
        }
        // Where the point is: a line from the middle, and the point.
        Rectangle {
            readonly property real len: Math.hypot(wheel.pointX, wheel.pointY) * disc.radius
            visible: len > 1
            x: disc.centre.x; y: disc.centre.y - height / 2
            width: len; height: 1
            color: Theme.textPrimary
            opacity: 0.5
            transformOrigin: Item.Left
            rotation: -Math.atan2(wheel.pointY, wheel.pointX) * 180 / Math.PI
        }
        Rectangle {
            objectName: "colourWheelPoint_" + wheel.name
            width: Theme.s3; height: width; radius: width / 2
            x: disc.centre.x + wheel.pointX * disc.radius - width / 2
            y: disc.centre.y - wheel.pointY * disc.radius - height / 2
            color: "transparent"
            border.color: area.pressed ? Theme.accent : Theme.textPrimary
            border.width: 2
        }
        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.CrossCursor
            property point last
            onPressed: (mouse) => last = Qt.point(mouse.x, mouse.y)
            onPositionChanged: (mouse) => {
                if (!pressed) return
                const fine = (mouse.modifiers & Qt.ShiftModifier) ? 0.2 : 1
                let x = wheel.pointX + (mouse.x - last.x) / disc.radius * fine
                let y = wheel.pointY - (mouse.y - last.y) / disc.radius * fine
                const d = Math.hypot(x, y)
                if (d > 1) { x /= d; y /= d }
                last = Qt.point(mouse.x, mouse.y)
                wheel.pointMoved(x, y, true)
            }
            onReleased: wheel.finished()
            onDoubleClicked: { wheel.pointMoved(0, 0, false); wheel.finished() }
        }
    }
    Slider {
        objectName: "colourWheelLevel_" + wheel.name
        Layout.fillWidth: true
        from: wheel.minimum; to: wheel.maximum
        value: wheel.level
        onMoved: wheel.levelMoved(value, true)
        onPressedChanged: if (!pressed) wheel.finished()
        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
        ToolTip.text: wheel.tip
        Accessible.name: qsTr("%1 level").arg(wheel.label)
    }
}
