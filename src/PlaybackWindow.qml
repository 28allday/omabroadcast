import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import OmaBroadcast 1.0

// The last recording, played back to check it: picture, sound meters, the
// file's details and shuttle controls. Nothing here reaches the engine or the
// outputs: see core/recording_player.h. Like the prompter, the desktop gives it
// no title bar, so it brings its own bar to move it by and edges to resize it by.
Window {
    id: playback
    objectName: "playbackWindow"
    // Not translated: Hyprland's rule finds the window by this title.
    title: "OmaBroadcast playback"
    width: 1040
    height: 680
    minimumWidth: 640
    minimumHeight: 420
    visible: true
    color: Theme.showBg

    readonly property var player: backend.player
    readonly property bool hasFile: player.path !== ""
    property bool showInfo: false
    FontMetrics { id: captionMetrics; font.pixelSize: Theme.fsCaption }

    onClosing: backend.playbackOpen = false

    // HH:MM:SS:FF at the file's own frame rate.
    function timecode(seconds) {
        const fps = Math.max(1, Math.round(player.frameRate))
        const frames = Math.round(Math.max(0, seconds) * player.frameRate)
        const f = frames % fps, s = Math.floor(frames / fps)
        const two = (n) => (n < 10 ? "0" : "") + n
        return two(Math.floor(s / 3600)) + ":" + two(Math.floor(s / 60) % 60) + ":" + two(s % 60) + ":" + two(f)
    }

    Shortcut { sequence: "Space"; onActivated: player.togglePlay() }
    Shortcut { sequence: "K"; onActivated: player.stop() }
    Shortcut { sequence: "L"; onActivated: player.forward() }
    Shortcut { sequence: "J"; onActivated: player.backward() }
    Shortcut { sequence: "Left"; onActivated: player.stepFrames(-1) }
    Shortcut { sequence: "Right"; onActivated: player.stepFrames(1) }
    Shortcut { sequence: "Shift+Left"; onActivated: player.stepFrames(-Math.max(1, Math.round(player.frameRate))) }
    Shortcut { sequence: "Shift+Right"; onActivated: player.stepFrames(Math.max(1, Math.round(player.frameRate))) }
    Shortcut { sequence: "Home"; onActivated: { player.stop(); player.toStart() } }
    Shortcut { sequence: "End"; onActivated: player.toEnd() }
    Shortcut { sequence: "I"; onActivated: playback.showInfo = !playback.showInfo }
    Shortcut { sequence: "Escape"; onActivated: backend.playbackOpen = false }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            objectName: "playbackMoveBar"
            Layout.fillWidth: true
            implicitHeight: closeButton.implicitHeight + Theme.s1 * 2
            color: Theme.panelBg
            MouseArea {
                anchors.fill: parent
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                onPressed: playback.startSystemMove()
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.s3
                anchors.rightMargin: Theme.s1
                spacing: Theme.s1
                Label {
                    objectName: "playbackTitle"
                    Layout.fillWidth: true
                    text: playback.hasFile ? qsTr("Playback: %1").arg(player.fileName) : qsTr("Playback")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fsLabel
                    elide: Text.ElideMiddle
                }
                Button {
                    objectName: "playbackInfo"
                    flat: true
                    checkable: true
                    checked: playback.showInfo
                    enabled: playback.hasFile
                    icon.name: "info"
                    ToolTip.text: qsTr("The file's details (I)")
                    onToggled: playback.showInfo = checked
                }
                Button {
                    objectName: "playbackShowFolder"
                    flat: true
                    icon.name: "folder-open"
                    ToolTip.text: qsTr("Open the recordings folder")
                    onClicked: backend.showRecordings()
                }
                Button {
                    id: closeButton
                    objectName: "playbackClose"
                    flat: true
                    icon.name: "x"
                    ToolTip.text: qsTr("Close playback (Escape)")
                    onClicked: backend.playbackOpen = false
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // The picture, its own shape, on black.
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "black"
                PreviewView {
                    id: picture
                    objectName: "playbackPicture"
                    readonly property real shape: player.videoWidth > 0 && player.videoHeight > 0 ? player.videoWidth / player.videoHeight : 16 / 9
                    width: Math.min(parent.width, parent.height * shape)
                    height: width / shape
                    anchors.centerIn: parent
                    visible: playback.hasFile && player.problem === ""
                    Component.onCompleted: player.view = picture
                    Component.onDestruction: if (player.view === picture) player.view = null
                }
                Label {
                    objectName: "playbackEmpty"
                    anchors.centerIn: parent
                    width: parent.width - Theme.s6 * 2
                    visible: !picture.visible
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.textSecondary
                    text: player.problem !== "" ? player.problem
                        : qsTr("No recording yet. Record something, and it opens here when it is saved.")
                }
            }

            // The sound: a meter a channel, as the mixer's.
            Rectangle {
                objectName: "playbackMeters"
                Layout.fillHeight: true
                implicitWidth: Math.max(96, player.channels * (Theme.hMeter + Theme.s2) + 48)
                color: Theme.panelBg
                visible: playback.hasFile && player.problem === ""
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.s2
                    spacing: Theme.s2
                    RowLayout {
                        Layout.fillHeight: true
                        Layout.alignment: Qt.AlignHCenter
                        spacing: Theme.s2
                        // The scale: dB marks where the meters put them (-60 to 0).
                        Item {
                            Layout.fillHeight: true
                            // Level with the meters, which have their channel's name beneath.
                            Layout.bottomMargin: captionMetrics.height + Theme.s1
                            implicitWidth: 24
                            Repeater {
                                model: [0, -6, -12, -20, -30, -40, -60]
                                Text {
                                    required property int modelData
                                    anchors.right: parent.right
                                    y: parent.height * (-modelData / 60) - height / 2
                                    text: modelData
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fsCaption
                                }
                            }
                        }
                        Repeater {
                            model: player.channels
                            ColumnLayout {
                                required property int index
                                Layout.fillHeight: true
                                spacing: Theme.s1
                                LevelMeter {
                                    objectName: "playbackMeter-" + index
                                    vertical: true
                                    Layout.fillHeight: true
                                    level: player.meter.levels?.[index] ?? 0
                                    clipped: player.meter.clipped ?? false
                                }
                                Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: player.channels === 2 ? (index === 0 ? "L" : "R") : String(index + 1)
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fsCaption
                                }
                            }
                        }
                    }
                    // The loudest so far; click to start counting again.
                    Label {
                        objectName: "playbackPeak"
                        Layout.alignment: Qt.AlignHCenter
                        horizontalAlignment: Text.AlignHCenter
                        readonly property real peak: player.meter.maximumDb ?? -120
                        text: peak <= -119 ? qsTr("Peak\n–") : qsTr("Peak\n%1 dB").arg(peak.toFixed(1))
                        color: (player.meter.clipped ?? false) ? Theme.danger : Theme.textSecondary
                        font.pixelSize: Theme.fsCaption
                        font.family: Theme.monoFamily
                        MouseArea { anchors.fill: parent; onClicked: player.resetPeak() }
                        ToolTip.visible: peakHover.hovered
                        ToolTip.text: qsTr("The loudest moment so far; red if it reached 0 dB. Click to start again.")
                        HoverHandler { id: peakHover }
                    }
                }
            }

            // What the file is.
            Rectangle {
                objectName: "playbackDetails"
                Layout.fillHeight: true
                implicitWidth: 300
                visible: playback.showInfo && playback.hasFile
                color: Theme.panelBg
                ListView {
                    anchors.fill: parent
                    anchors.margins: Theme.s3
                    clip: true
                    spacing: Theme.s2
                    model: player.info
                    delegate: ColumnLayout {
                        required property var modelData
                        width: ListView.view.width
                        spacing: 0
                        Label { text: modelData.label; color: Theme.textMuted; font.pixelSize: Theme.fsCaption }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.value
                            color: Theme.textPrimary
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                        }
                    }
                }
            }
        }

        // Where it is, and the shuttle.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: transport.implicitHeight + Theme.s2 * 2
            color: Theme.panelBg
            ColumnLayout {
                id: transport
                anchors.fill: parent
                anchors.margins: Theme.s2
                anchors.leftMargin: Theme.s3
                anchors.rightMargin: Theme.s3
                spacing: Theme.s1
                enabled: playback.hasFile && player.problem === ""

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s2
                    Label {
                        objectName: "playbackTime"
                        text: playback.timecode(player.position)
                        font.family: Theme.monoFamily
                        color: Theme.textPrimary
                    }
                    Slider {
                        id: scrub
                        objectName: "playbackScrub"
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(0.001, player.duration)
                        value: player.position
                        onMoved: { player.stop(); player.seek(value) }
                        ToolTip.text: qsTr("Drag to look through the recording")
                    }
                    Label {
                        objectName: "playbackDuration"
                        text: playback.timecode(player.duration)
                        font.family: Theme.monoFamily
                        color: Theme.textSecondary
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s1
                    Label {
                        objectName: "playbackSound"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        text: player.soundNote
                        color: Theme.warning
                        font.pixelSize: Theme.fsCaption
                        wrapMode: Text.Wrap
                    }
                    Button { objectName: "playbackStart"; flat: true; icon.name: "skip-back"; ToolTip.text: qsTr("To the start (Home)"); onClicked: { player.stop(); player.toStart() } }
                    Button { objectName: "playbackBackward"; flat: true; icon.name: "rewind"; highlighted: player.shuttle < 0
                             ToolTip.text: qsTr("Back: press again for faster (J)"); onClicked: player.backward() }
                    Button { objectName: "playbackFrameBack"; flat: true; icon.name: "chevron-left"; ToolTip.text: qsTr("One frame back (Left; Shift+Left a second)"); onClicked: player.stepFrames(-1) }
                    Button {
                        objectName: "playbackPlay"
                        icon.name: player.shuttle !== 0 ? "pause" : "play"
                        ToolTip.text: player.shuttle !== 0 ? qsTr("Stop (Space or K)") : qsTr("Play (Space or L)")
                        onClicked: player.togglePlay()
                    }
                    Button { objectName: "playbackFrameForward"; flat: true; icon.name: "chevron-right"; ToolTip.text: qsTr("One frame on (Right; Shift+Right a second)"); onClicked: player.stepFrames(1) }
                    Button { objectName: "playbackForward"; flat: true; icon.name: "fast-forward"; highlighted: player.shuttle > 1
                             ToolTip.text: qsTr("Forward: press again for faster (L)"); onClicked: player.forward() }
                    Button { objectName: "playbackEnd"; flat: true; icon.name: "skip-forward"; ToolTip.text: qsTr("To the end (End)"); onClicked: player.toEnd() }
                    Label {
                        objectName: "playbackShuttle"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        text: player.shuttle === 0 || player.shuttle === 1 ? "" : (player.shuttle < 0 ? "−" : "") + Math.abs(player.shuttle) + "×"
                        color: Theme.accent
                        font.family: Theme.monoFamily
                    }
                }
            }
        }
    }

    // Above everything, at the edges only.
    WindowEdges { window: playback; prefix: "playback" }
}
