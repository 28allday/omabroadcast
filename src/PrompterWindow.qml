import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import OmaBroadcast 1.0

// The script in a window of its own, to be put beside the camera or over the
// picture in the main window. Nothing here reaches the engine: see
// backendprompter.cpp. The desktop gives it no title bar, so it brings its own bar
// to move it by and edges to resize it by, both handed to the compositor.
Window {
    id: cue
    objectName: "prompterWindow"
    // Not translated: Hyprland's rule finds the window by this title.
    title: "OmaBroadcast prompter"
    width: backend.prompterWidth
    height: backend.prompterHeight
    minimumWidth: 560
    minimumHeight: 240
    visible: true
    color: Theme.showBg

    readonly property bool hasScript: backend.prompterText !== ""
    readonly property int lineHeight: Math.round(backend.prompterSize * 1.3)
    // Speed is lines in ten seconds, so a larger size keeps the same pace of words.
    readonly property real pixelsPerSecond: backend.prompterSpeed * lineHeight / 10
    // Where the eye rests, and so where the first line starts and the last line ends.
    readonly property real readingLine: script.height / 3

    onClosing: backend.prompterOpen = false
    onWidthChanged: remember.restart()
    onHeightChanged: remember.restart()
    Timer { id: remember; interval: 500; onTriggered: backend.setPrompterWindowSize(cue.width, cue.height) }

    Connections {
        target: backend
        function onPrompterRestarted() { script.contentY = 0 }
    }

    Shortcut { sequence: "Space"; onActivated: backend.prompterPlaying = !backend.prompterPlaying }
    Shortcut { sequence: "Up"; onActivated: backend.prompterSpeed = backend.prompterSpeed + 1 }
    Shortcut { sequence: "Down"; onActivated: backend.prompterSpeed = backend.prompterSpeed - 1 }
    Shortcut { sequence: "Home"; onActivated: backend.restartPrompter() }
    Shortcut { sequence: "Escape"; onActivated: backend.prompterOpen = false }

    FrameAnimation {
        running: backend.prompterPlaying && cue.visible
        onTriggered: {
            const end = script.contentHeight - script.height
            const next = script.contentY + cue.pixelsPerSecond * frameTime
            script.contentY = Math.min(next, end)
            if (next >= end) backend.prompterPlaying = false
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            objectName: "prompterMoveBar"
            Layout.fillWidth: true
            implicitHeight: closeButton.implicitHeight + Theme.s1 * 2
            color: Theme.panelBg
            MouseArea {
                anchors.fill: parent
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                onPressed: cue.startSystemMove()
            }
            Label {
                anchors.left: parent.left
                anchors.leftMargin: Theme.s3
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Prompter: drag here to move, drag an edge to resize")
                color: Theme.textSecondary
                font.pixelSize: Theme.fsLabel
            }
            Button {
                id: closeButton
                objectName: "prompterClose"
                anchors.right: parent.right
                anchors.rightMargin: Theme.s1
                anchors.verticalCenter: parent.verticalCenter
                flat: true
                icon.name: "x"
                ToolTip.text: qsTr("Close the prompter (Escape)")
                onClicked: backend.prompterOpen = false
            }
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Flickable {
                id: script
                objectName: "prompterScript"
                anchors.fill: parent
                clip: true
                visible: cue.hasScript
                contentWidth: width
                // Room above and below, so that the first line starts on the reading
                // line and the last can come to rest on it.
                contentHeight: words.implicitHeight + height
                boundsBehavior: Flickable.StopAtBounds
                // Wheel or drag to find a place; it goes on from there.
                Text {
                    id: words
                    objectName: "prompterWords"
                    x: Theme.s6
                    y: cue.readingLine
                    width: script.width - Theme.s6 * 2
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    text: backend.prompterText
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: backend.prompterSize
                    lineHeightMode: Text.FixedHeight
                    lineHeight: cue.lineHeight
                }
            }
            // The reading line: a mark at each side, clear of the words.
            Repeater {
                model: cue.hasScript ? 2 : 0
                Rectangle {
                    required property int index
                    x: index === 0 ? Theme.s2 : parent.width - width - Theme.s2
                    y: cue.readingLine + cue.lineHeight / 2 - height / 2
                    width: Theme.s4
                    height: Theme.s1
                    color: Theme.accent
                }
            }
            Label {
                objectName: "prompterEmpty"
                anchors.centerIn: parent
                width: parent.width - Theme.s6 * 2
                visible: !cue.hasScript
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                color: Theme.textSecondary
                text: backend.prompterProblem !== "" ? backend.prompterProblem : qsTr("Load a script to begin: a text or Markdown file.")
            }
        }
        Label {
            objectName: "prompterCaptureNote"
            Layout.fillWidth: true
            Layout.margins: Theme.s2
            visible: !backend.prompterHidden
            wrapMode: Text.Wrap
            color: Theme.warning
            font.pixelSize: Theme.fsLabel
            text: qsTr("A capture of this screen can show this window here. Keep it on a screen you are not capturing.")
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: controls.implicitHeight + Theme.s2 * 2
            color: Theme.panelBg
            // The gaps between the buttons move it too.
            MouseArea {
                anchors.fill: parent
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                onPressed: cue.startSystemMove()
            }
            RowLayout {
                id: controls
                anchors.fill: parent
                anchors.margins: Theme.s2
                spacing: Theme.s2
                Button {
                    objectName: "prompterLoad"
                    text: qsTr("Load…")
                    icon.name: "folder-open"
                    ToolTip.text: qsTr("Choose a text or Markdown file")
                    onClicked: backend.choosePrompterFile()
                }
                Button {
                    objectName: "prompterPlay"
                    enabled: cue.hasScript
                    text: backend.prompterPlaying ? qsTr("Pause") : qsTr("Play")
                    icon.name: backend.prompterPlaying ? "pause" : "play"
                    highlighted: true
                    ToolTip.text: qsTr("Start or pause the words (Space)")
                    onClicked: backend.prompterPlaying = !backend.prompterPlaying
                }
                Button {
                    objectName: "prompterRestart"
                    enabled: cue.hasScript
                    icon.name: "rotate-ccw"
                    ToolTip.text: qsTr("Back to the start (Home)")
                    onClicked: backend.restartPrompter()
                }
                Item { Layout.fillWidth: true }
                Label { text: qsTr("Speed %1").arg(backend.prompterSpeed); color: Theme.textSecondary }
                Button {
                    objectName: "prompterSlower"
                    Layout.preferredWidth: implicitHeight
                    text: "−"
                    ToolTip.text: qsTr("Slower (Down)")
                    onClicked: backend.prompterSpeed = backend.prompterSpeed - 1
                }
                Button {
                    objectName: "prompterFaster"
                    Layout.preferredWidth: implicitHeight
                    text: "+"
                    ToolTip.text: qsTr("Faster (Up)")
                    onClicked: backend.prompterSpeed = backend.prompterSpeed + 1
                }
                Label { text: qsTr("Size %1").arg(backend.prompterSize); color: Theme.textSecondary }
                Button {
                    objectName: "prompterSmaller"
                    Layout.preferredWidth: implicitHeight
                    text: "−"
                    ToolTip.text: qsTr("Smaller words")
                    onClicked: backend.prompterSize = backend.prompterSize - 4
                }
                Button {
                    objectName: "prompterBigger"
                    Layout.preferredWidth: implicitHeight
                    text: "+"
                    ToolTip.text: qsTr("Bigger words")
                    onClicked: backend.prompterSize = backend.prompterSize + 4
                }
            }
        }
    }

    // Above everything, at the edges only, so that a drag on the words still scrolls.
    WindowEdges { window: cue; prefix: "prompter" }
}
