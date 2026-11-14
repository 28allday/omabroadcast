import QtQuick
import qs.Commons
import qs.Commons as Commons

// The right-click menu: what the app's tray menu offers.
Item {
  id: panel

  property var widget: null
  property QtObject bar: null

  readonly property var status: widget ? widget.status : ({})
  readonly property color fg: bar ? bar.foreground : Commons.Color.foreground
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family

  // The rows are fixed and each reads the status for itself, so a new status
  // (one a second) changes a row's words and never the row: a click that
  // straddles it still lands, and the highlight under the pointer stays.
  readonly property var rows: ["window", "record", "stream", "replay", "replaySave", "camera", "quit"]

  function rowText(name, s) {
    if (name === "window") return s.windowVisible ? "Hide window" : "Show window"
    if (name === "record") return s.recordingStopping ? "Finishing recording…" : s.recording ? "Stop recording" : "Start recording"
    if (name === "stream") return s.streamingStopping ? "Ending the stream…" : s.streaming ? "End stream" : "Go live"
    if (name === "replay") return s.replay ? "Stop the replay buffer" : "Start the replay buffer"
    if (name === "replaySave") return "Save replay"
    if (name === "camera") return s.virtualCamera ? "Stop the virtual camera" : "Start the virtual camera"
    return "Quit"
  }
  function rowOff(name, s) {
    return (name === "record" && s.recordingStopping === true) || (name === "stream" && s.streamingStopping === true)
  }
  // Save replay is there only while the buffer runs.
  function rowShown(name, s) { return name !== "replaySave" || s.replay === true }

  implicitHeight: column.implicitHeight + Style.space(16)

  Column {
    id: column
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.top: parent.top
    anchors.margins: Style.space(8)

    Repeater {
      model: panel.rows
      delegate: Rectangle {
        id: row
        required property string modelData
        readonly property bool off: panel.rowOff(modelData, panel.status)
        visible: panel.rowShown(modelData, panel.status)
        width: column.width
        height: visible ? Style.space(30) : 0
        radius: Style.space(4)
        color: mouse.containsMouse && !row.off ? Qt.rgba(panel.fg.r, panel.fg.g, panel.fg.b, 0.12) : "transparent"

        Text {
          anchors.verticalCenter: parent.verticalCenter
          anchors.left: parent.left
          anchors.right: parent.right
          anchors.leftMargin: Style.space(8)
          anchors.rightMargin: Style.space(8)
          text: panel.rowText(row.modelData, panel.status)
          textFormat: Text.PlainText
          elide: Text.ElideRight
          color: panel.fg
          opacity: row.off ? 0.5 : 1
          font.family: panel.fontFamily
          font.pixelSize: Style.font.body
        }
        MouseArea {
          id: mouse
          anchors.fill: parent
          hoverEnabled: true
          enabled: !row.off
          onClicked: {
            panel.widget.command(row.modelData)
            panel.widget.close()
          }
        }
      }
    }
  }
}
