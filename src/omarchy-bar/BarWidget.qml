import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui

// OmaBroadcast in the bar. It asks the running app over its control socket once
// a second, and the app acts on what is pressed here as it does on its own
// menus, confirmations included. With the app closed a click starts it.
BarWidget {
  id: root
  moduleName: "nosignal.omabroadcast"

  property bool opened: false
  property bool popoutSwitchClosing: false
  property var status: ({})

  function open() { opened = true }
  function close() { opened = false }
  function toggle() { opened = !opened }

  readonly property bool running: control.connected && status.recording !== undefined
  readonly property bool recording: running && status.recording === true
  readonly property bool streaming: running && status.streaming === true
  readonly property bool onAir: recording || streaming
  readonly property string time: recording ? String(status.recordingTime || "") : streaming ? String(status.streamingTime || "") : ""

  function send(action) {
    if (!control.connected) return
    control.write(JSON.stringify({ action: action }) + "\n")
    control.flush()
  }
  function command(name) {
    send("bar." + name)
    send("status")
  }
  function launch() {
    if (root.bar) root.bar.run("omabroadcast")
  }

  Socket {
    id: control
    path: (Quickshell.env("XDG_RUNTIME_DIR") || "") + "/omabroadcast.sock"
    onConnectedChanged: {
      if (connected) root.send("status")
      else { root.status = {}; root.opened = false }
    }
    parser: SplitParser {
      onRead: (line) => {
        var reply
        try { reply = JSON.parse(line) } catch (e) { return }
        if (reply && reply.ok === true && reply.result && reply.result.recording !== undefined) root.status = reply.result
      }
    }
  }
  // A connection tried with nothing there is a line in the shell's log: looked
  // for first, every few seconds, and only while the app is closed. The socket's
  // file alone is not enough: a copy that crashed leaves it behind, so the app
  // is asked whether it answers (where it is not on the PATH, the file decides).
  Process {
    id: lookForApp
    command: ["sh", "-c",
              "test -S \"$1\" || exit 1; command -v omabroadcast >/dev/null 2>&1 || exit 0; exec omabroadcast ctl status >/dev/null 2>&1",
              "sh", control.path]
    onExited: (code) => { if (code === 0) control.connected = true }
  }
  Timer {
    interval: 3000
    running: !control.connected
    repeat: true
    triggeredOnStart: true
    onTriggered: if (!lookForApp.running) lookForApp.running = true
  }
  Timer {
    interval: 1000
    running: control.connected
    repeat: true
    onTriggered: root.send("status")
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  TextMetrics {
    id: timeWidth
    font.family: button.fontFamily
    font.pixelSize: button.fontSize
    text: root.time
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "󱜠"
    labelText: root.vertical ? "" : root.time
    // The button keeps to one icon's width unless told; the time needs its own.
    fixedWidth: root.vertical ? -1 : Style.bar.iconSlot + (labelText === "" ? 0 : timeWidth.advanceWidth + Style.space(8))
    useActiveColor: false
    foreground: root.onAir ? Commons.Color.urgent : root.running ? Commons.Color.accent : Commons.Color.muted
    tooltipText: !root.running ? "OmaBroadcast: click to open"
               : root.recording && root.streaming ? "OmaBroadcast: recording and live"
               : root.recording ? "OmaBroadcast: recording"
               : root.streaming ? "OmaBroadcast: live"
               : "OmaBroadcast"
    onPressed: function(pressedButton) {
      if (!root.running) root.launch()
      else if (pressedButton === Qt.RightButton) root.toggle()
      else root.command("window")
    }
  }

  KeyboardPanel {
    id: kpanel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: kpanel.fittedContentWidth(Style.space(240))
    contentHeight: kpanel.fittedContentHeight(contentLoader.item ? contentLoader.item.implicitHeight : Style.space(200))

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.close()

      Loader {
        id: contentLoader
        anchors.fill: parent
        source: "Panel.qml"
        onLoaded: {
          if (!item) return
          item.widget = root
          item.bar = root.bar
        }
      }
    }
  }
}
