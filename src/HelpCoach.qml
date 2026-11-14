import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Walks someone through a task from the guide on the real window: a small
// card in the corner says what to do, and a ring goes round the control to
// use. A step with a done check moves on by itself; the others wait for Next.
//
// The ring is drawn on the overlay above everything, sheets included, and
// lets every click through, so the control under it works as normal.
Item {
    id: coach
    objectName: "helpCoach"
    parent: Overlay.overlay
    anchors.fill: parent
    z: 10000
    visible: running || pointing

    // Where to look for the controls the steps point at: the main window's content.
    property Item scope: null

    signal finished(string key)
    signal openGuide(string key)

    property var task: null
    property int step: 0
    property bool complete: false
    readonly property bool running: task !== null
    readonly property var current: running && !complete ? task.steps[step] : null
    // What a step's check wants remembered between looks (a recording seen started).
    property var run: ({ seen: false })

    // "Show me where" from a part of the window: the ring alone, for a few seconds.
    property bool pointing: false
    property string pointAt: ""
    readonly property string target: pointing ? pointAt : (current?.target ?? "")

    function start(t) {
        task = t; step = 0; complete = false; run = ({ seen: false, step: ({}) }); pointing = false
        look()
    }
    function stop() { task = null; complete = false }
    function next() {
        if (!running) return
        if (step + 1 < task.steps.length) { step += 1; run.step = ({}); look() }
        else complete = true
    }
    function back() { if (running && step > 0) { step -= 1; complete = false; look() } }
    function point(name) { stop(); pointAt = name; pointing = true; pointTimer.restart(); look() }

    // The control a step points at, by objectName, if it is on screen now.
    function find(item, name) {
        if (!item) return null
        if (item.objectName === name) return item.visible ? item : null
        const kids = item.children
        for (let i = 0; i < kids.length; ++i) {
            const hit = find(kids[i], name)
            if (hit) return hit
        }
        return null
    }
    // In the window, or in a sheet open in front of it.
    function locate(name) {
        const kids = coach.parent ? coach.parent.children : []
        for (let i = 0; i < kids.length; ++i) {
            if (kids[i] === coach) continue
            const hit = find(kids[i], name)
            if (hit) return hit
        }
        return find(scope, name)
    }
    // For a step's check: whether a control is on screen now.
    function shown(name) { return locate(name) !== null }
    property rect ring: Qt.rect(0, 0, 0, 0)
    property bool ringFound: false
    function look() {
        if (current && current.done && current.done(backend, run, coach)) { next(); return }
        const t = target === "" ? null : locate(target)
        ringFound = t !== null && t.width > 0
        if (ringFound) {
            const p = t.mapToItem(coach, 0, 0)
            ring = Qt.rect(p.x, p.y, t.width, t.height)
        }
    }
    // Controls move as panels open and the window resizes; look again often.
    Timer { interval: 150; repeat: true; running: coach.visible; onTriggered: coach.look() }
    Timer { id: pointTimer; interval: 4000; onTriggered: coach.pointing = false }

    // Everything but the control is dimmed, so the eye goes to it. Drawing
    // only: clicks pass through to the window as normal.
    Item {
        id: spotlight
        anchors.fill: parent
        // A sheet in front already shades the window; shading it again would hide the sheet.
        visible: ringShape.visible && Sheets.count === 0
        readonly property color shade: Theme.withAlpha(Theme.windowBg, 0.55)
        readonly property real l: ringShape.x
        readonly property real t: ringShape.y
        readonly property real r: ringShape.x + ringShape.width
        readonly property real b: ringShape.y + ringShape.height
        Rectangle { color: spotlight.shade; x: 0; y: 0; width: parent.width; height: Math.max(0, spotlight.t) }
        Rectangle { color: spotlight.shade; x: 0; y: spotlight.b; width: parent.width; height: Math.max(0, parent.height - spotlight.b) }
        Rectangle { color: spotlight.shade; x: 0; y: spotlight.t; width: Math.max(0, spotlight.l); height: spotlight.b - spotlight.t }
        Rectangle { color: spotlight.shade; x: spotlight.r; y: spotlight.t; width: Math.max(0, parent.width - spotlight.r); height: spotlight.b - spotlight.t }
    }

    Rectangle {
        id: ringShape
        objectName: "helpCoachRing"
        visible: coach.ringFound && coach.target !== "" && !coach.complete
        x: coach.ring.x - Theme.s1
        y: coach.ring.y - Theme.s1
        width: coach.ring.width + Theme.s1 * 2
        height: coach.ring.height + Theme.s1 * 2
        color: "transparent"
        radius: Theme.rControl + 2
        border.color: Theme.accent
        border.width: Theme.selectionRing + 1
    }

    // The card. Its own mouse area keeps clicks on it from reaching what is under it.
    Rectangle {
        id: card
        objectName: "helpCoachCard"
        visible: coach.running
        // On the side away from the control pointed at, so it never covers it.
        readonly property bool onLeft: coach.ringFound && coach.ring.x + coach.ring.width / 2 > coach.width / 2
        x: onLeft ? Theme.s5 : coach.width - width - Theme.s5
        Behavior on x { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.hStatusBar + Theme.s4
        width: 360
        height: cardBody.implicitHeight + Theme.s4 * 2
        color: Theme.panelRaised
        border.color: Theme.borderStrong
        radius: Theme.rCard
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }

        ColumnLayout {
            id: cardBody
            anchors.fill: parent
            anchors.margins: Theme.s4
            spacing: Theme.s2

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: coach.running ? coach.task.name : ""
                    color: Theme.textMuted
                    font.pixelSize: Theme.fsCaption
                    elide: Text.ElideRight
                }
                Label {
                    objectName: "helpCoachCount"
                    visible: !coach.complete
                    text: coach.running ? qsTr("Step %1 of %2").arg(coach.step + 1).arg(coach.task.steps.length) : ""
                    color: Theme.textMuted
                    font.pixelSize: Theme.fsCaption
                }
                ToolButton {
                    icon.name: "x"
                    implicitWidth: Theme.hControl; implicitHeight: Theme.hControl
                    onClicked: coach.stop()
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Stop the walk-through")
                    Accessible.name: qsTr("Stop the walk-through")
                }
            }
            // Progress, one mark a step.
            RowLayout {
                Layout.fillWidth: true
                spacing: 3
                Repeater {
                    model: coach.running ? coach.task.steps.length : 0
                    Rectangle {
                        required property int index
                        Layout.fillWidth: true
                        implicitHeight: 3
                        radius: 1.5
                        color: coach.complete || index < coach.step ? Theme.accent
                             : index === coach.step ? Theme.withAlpha(Theme.accent, 0.5) : Theme.controlBg
                    }
                }
            }
            Label {
                objectName: "helpCoachTitle"
                Layout.fillWidth: true
                Layout.topMargin: Theme.s1
                text: coach.complete ? qsTr("Done") : (coach.current?.title ?? "")
                color: Theme.textPrimary
                font.pixelSize: Theme.fsSection
                font.weight: Theme.wHeading
                wrapMode: Text.WordWrap
            }
            Label {
                objectName: "helpCoachText"
                Layout.fillWidth: true
                text: coach.complete ? (coach.task?.finish ?? "") : (coach.current?.text ?? "")
                color: Theme.textSecondary
                wrapMode: Text.WordWrap
                lineHeight: 1.1
            }
            Label {
                Layout.fillWidth: true
                visible: !coach.complete && coach.current !== null && coach.current.done !== undefined
                text: qsTr("This moves on by itself when you have done it.")
                color: Theme.textMuted
                font.pixelSize: Theme.fsCaption
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.s1
                spacing: Theme.s2
                Button {
                    flat: true
                    visible: !coach.complete
                    enabled: coach.step > 0
                    text: qsTr("Back")
                    ToolTip.text: qsTr("The step before")
                    onClicked: coach.back()
                }
                Button {
                    flat: true
                    visible: !coach.complete
                    text: qsTr("Guide")
                    ToolTip.text: qsTr("Open this task in the guide")
                    onClicked: coach.openGuide(coach.task.key)
                }
                Item { Layout.fillWidth: true }
                Button {
                    objectName: "helpCoachShowRecordings"
                    ToolTip.text: qsTr("Open the folder your recordings are saved in")
                    visible: coach.complete && coach.task?.key === "first-recording"
                    text: qsTr("Show my recordings")
                    icon.name: "folder-open"
                    onClicked: backend.showRecordings()
                }
                Button {
                    objectName: "helpCoachNext"
                    ToolTip.text: qsTr("Go on to the next step")
                    highlighted: true
                    // A step that checks for itself can still be skipped.
                    text: coach.complete ? qsTr("Close")
                        : coach.current?.done !== undefined ? qsTr("Skip")
                        : coach.step + 1 === (coach.task?.steps.length ?? 0) ? qsTr("Finish") : qsTr("Next")
                    flat: !coach.complete && coach.current?.done !== undefined
                    onClicked: {
                        if (coach.complete) { const k = coach.task.key; coach.stop(); coach.finished(k) }
                        else coach.next()
                    }
                }
            }
        }
    }
}
