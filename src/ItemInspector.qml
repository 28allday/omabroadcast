import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// The selected item: its source's own settings, then where it sits.
ColumnLayout {
    id: insp
    spacing: Theme.s2
    readonly property var s: backend.selected
    readonly property var playback: backend.selectedMedia
    readonly property var set: s.settings ?? ({})
    readonly property string kind: s.kind ?? ""
    readonly property bool audioOnly: ["audio-input", "audio-output", "audio-application"].includes(kind)
    readonly property bool capture: kind === "display" || kind === "window" || kind === "region"
    readonly property bool network: kind === "media" && insp.set.port !== undefined
    readonly property bool file: kind === "media" && !network
    readonly property string passphrase: set.passphrase ?? ""
    readonly property bool rtmp: (set.protocol ?? "srt") === "rtmp"
    property string passphraseProblem: ""
    readonly property string sourceId: s.sourceId ?? ""
    onSourceIdChanged: passphraseProblem = ""

    function setting(key, value, gesture) { backend.setSourceSetting(insp.s.sourceId, key, value, gesture === true) }

    // A device's picture controls. The rows follow which controls it has and
    // not what they are set to: each new value is read back from the device,
    // and must not rebuild the slider under the mouse. A row reads its values by id.
    readonly property string controlShape: kind === "device" ? JSON.stringify((s.controls ?? []).map(c => [c.id, c.type, c.menu])) : "[]"
    readonly property var controlIds: JSON.parse(controlShape).map(c => c[0])
    function controlOf(id) { return (s.controls ?? []).find(c => c.id === id) ?? ({}) }

    component Pair: RowLayout {
        Layout.fillWidth: true
        spacing: Theme.s2
    }
    // Something to type into the phone: to read, select, or copy.
    component SendToLine: RowLayout {
        id: line
        property string text
        property string name
        Layout.fillWidth: true
        spacing: Theme.s2
        TextEdit {
            Layout.fillWidth: true
            text: line.text
            readOnly: true
            selectByMouse: true
            color: Theme.textPrimary
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsControl
            wrapMode: TextEdit.WrapAnywhere
        }
        ToolButton {
            icon.name: "copy"
            onClicked: backend.copyText(line.text)
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Copy"); Accessible.name: line.name
        }
    }

    TextField {
        ToolTip.text: qsTr("The source's name, as the Sources list shows it")
        objectName: "sourceName"
        Layout.fillWidth: true
        text: insp.s.name ?? ""
        font.weight: Theme.wHeading
        onEditingFinished: if (text !== insp.s.name) backend.renameSource(insp.s.sourceId, text)
        Accessible.name: qsTr("Source name")
    }

    // ── The source, in sections that fold away ─────────────────────────────
    InspectorSection {
        objectName: "sourceSection"
        key: "inspector.source"
        title: insp.kind === "text" ? qsTr("Text") : insp.kind === "image" ? qsTr("Image")
             : insp.kind === "slideshow" ? qsTr("Slideshow") : insp.kind === "studio" ? qsTr("Studio")
             : insp.kind === "group" ? qsTr("Group")
             : insp.kind === "colour" ? qsTr("Colour") : insp.kind === "device" ? qsTr("Capture device")
             : insp.kind === "browser" ? qsTr("Browser")
             : insp.network ? qsTr("Phone / network stream") : insp.file ? qsTr("Media file")
             : insp.audioOnly ? qsTr("Audio source") : qsTr("Capture")

        AudioTargetPicker {
            Layout.fillWidth: true
            visible: insp.audioOnly
            kind: insp.kind
            choice: insp.set.audioNode ?? ""
            choiceName: insp.set.audioLabel ?? insp.s.name ?? ""
            onChosen: (value) => insp.setting("audioNode", value)
        }

        // Network stream: what it listens on, and where to point the phone,
        // in the inspector's label column.
        Pair {
            visible: insp.network
            Label { text: qsTr("Protocol"); color: Theme.textSecondary; Layout.preferredWidth: Theme.wInspectorLabel }
            ComboBox {
                objectName: "networkProtocol"
                ToolTip.text: qsTr("How the phone sends: SRT copes better with Wi-Fi and mobile networks; RTMP suits apps that only offer it")
                Layout.fillWidth: true
                model: ["SRT", "RTMP"]
                readonly property var values: ["srt", "rtmp"]
                currentIndex: Math.max(0, values.indexOf(insp.set.protocol ?? "srt"))
                onActivated: backend.setNetworkStream(insp.s.sourceId, values[currentIndex], insp.set.port, insp.set.latency ?? 120)
                Accessible.name: qsTr("Protocol")
            }
        }
        NumField { label: qsTr("Port"); labelWidth: Theme.wInspectorLabel + Theme.s2; value: insp.set.port ?? 9000; Layout.fillWidth: true
                   visible: insp.network
                   ToolTip.text: qsTr("The port this computer listens on for the phone")
                   onCommitted: (v) => backend.setNetworkStream(insp.s.sourceId, insp.set.protocol ?? "srt", Math.round(v), insp.set.latency ?? 120) }
        NumField {
            ToolTip.text: qsTr("How long SRT waits for lost packets to be sent again. More copes with weaker connections and adds delay")
            visible: insp.network && (insp.set.protocol ?? "srt") === "srt"
            Layout.fillWidth: true
            label: qsTr("Latency"); labelWidth: Theme.wInspectorLabel + Theme.s2; suffix: "ms"; value: insp.set.latency ?? 120
            onCommitted: (v) => backend.setNetworkStream(insp.s.sourceId, "srt", insp.set.port, Math.round(v))
        }
        // The firewall would turn the phone away: say so, with the way in.
        Rectangle {
            visible: insp.network && (insp.s.firewall ?? "") !== ""
            Layout.fillWidth: true
            implicitHeight: fw.implicitHeight + Theme.s2 * 2
            radius: Theme.rControl
            color: Theme.withAlpha(Theme.warning, 0.10)
            border.width: Theme.hairline
            border.color: Theme.withAlpha(Theme.warning, 0.45)
            ColumnLayout {
                id: fw
                anchors.fill: parent
                anchors.margins: Theme.s2
                spacing: Theme.s1
                Label {
                    Layout.fillWidth: true
                    text: qsTr("The firewall will block the phone. To let it in, run this once in a terminal:")
                    color: Theme.textPrimary
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    TextEdit {
                        Layout.fillWidth: true
                        text: insp.s.firewall ?? ""
                        readOnly: true
                        selectByMouse: true
                        color: Theme.textPrimary
                        font.family: Theme.monoFamily
                    }
                    ToolButton {
                        icon.name: "copy"
                        onClicked: backend.copyText(insp.s.firewall)
                        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Copy"); Accessible.name: qsTr("Copy command")
                    }
                }
            }
        }
        // What a sender must have. Editable, and empty for an app that cannot do it.
        Pair {
            visible: insp.network
            Label { text: qsTr("Passphrase"); color: Theme.textSecondary; Layout.preferredWidth: Theme.wInspectorLabel }
            TextField {
                objectName: "networkPassphrase"
                ToolTip.text: insp.rtmp ? qsTr("A sender without it in its address is turned away. Clear it to turn this off, for an app that cannot do it")
                                        : qsTr("A sender without it is turned away, and what it sends is encrypted. Clear it to turn this off, for an app that cannot do it")
                Layout.fillWidth: true
                text: insp.passphrase
                font.family: Theme.monoFamily
                placeholderText: qsTr("None")
                onTextEdited: insp.passphraseProblem = ""
                onEditingFinished: if (text !== insp.passphrase) insp.passphraseProblem = backend.setNetworkPassphrase(insp.s.sourceId, text)
                Accessible.name: qsTr("Passphrase")
            }
            ToolButton {
                objectName: "networkPassphraseCopy"
                icon.name: "copy"
                enabled: insp.passphrase !== ""
                onClicked: backend.copyText(insp.passphrase)
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Copy"); Accessible.name: qsTr("Copy passphrase")
            }
        }
        Button {
            objectName: "networkNewPassphrase"
            visible: insp.network
            text: qsTr("New passphrase")
            ToolTip.text: qsTr("Make a new random one. The phone then needs the new one to send")
            onClicked: { insp.passphraseProblem = ""; backend.newNetworkPassphrase(insp.s.sourceId) }
        }
        Label {
            objectName: "networkPassphraseProblem"
            visible: insp.network && insp.passphraseProblem !== ""
            Layout.fillWidth: true
            text: insp.passphraseProblem
            color: Theme.warning
            font.pixelSize: Theme.fsLabel
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "networkOpenToAnyone"
            visible: insp.network && insp.passphrase === ""
            Layout.fillWidth: true
            text: qsTr("No passphrase: anyone who can reach this port can send a picture and sound here, on air if this source is showing. Use it only on networks you trust.")
            color: Theme.warning
            font.pixelSize: Theme.fsLabel
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "networkRtmpInClear"
            visible: insp.network && insp.passphrase !== "" && insp.rtmp
            Layout.fillWidth: true
            text: qsTr("RTMP checks the passphrase but encrypts nothing: anyone watching the network between the phone and this computer can read it, and see the picture. SRT does not have this problem.")
            color: Theme.warning
            font.pixelSize: Theme.fsLabel
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "networkSendTo"
            visible: insp.network
            Layout.fillWidth: true
            text: insp.passphrase === "" ? qsTr("Point the phone's streaming app at one of these, sending as caller:")
                  : insp.rtmp ? qsTr("Point the phone's streaming app at one of these. The passphrase is part of the address:")
                  : qsTr("Point the phone's streaming app at one of these, sending as caller, and type the passphrase into its passphrase field:")
            color: Theme.textSecondary
            font.pixelSize: Theme.fsLabel
            wrapMode: Text.Wrap
        }
        Repeater {
            model: insp.network ? (insp.s.sendTo ?? []) : []
            delegate: ColumnLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 0
                Label { text: modelData.label; color: Theme.textMuted; font.pixelSize: Theme.fsCaption }
                SendToLine { text: modelData.url; name: qsTr("Copy address") }
                Label {
                    visible: modelData.streamKey !== undefined
                    Layout.fillWidth: true
                    text: qsTr("Stream key, if the app asks for one: %1").arg(modelData.streamKey ?? "")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fsCaption
                    wrapMode: Text.Wrap
                }
                Label {
                    visible: modelData.inAddress !== undefined
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.s1
                    text: insp.rtmp ? qsTr("Or all in one, for an app with only an address:") : qsTr("Or all in one, for an app with no passphrase field:")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fsCaption
                    wrapMode: Text.Wrap
                }
                SendToLine { visible: modelData.inAddress !== undefined; text: modelData.inAddress ?? ""; name: qsTr("Copy address with passphrase") }
            }
        }

        // Media file
        Label {
            visible: insp.file
            Layout.fillWidth: true
            text: insp.set.input ?? ""
            color: Theme.textSecondary
            elide: Text.ElideMiddle
        }
        Button {
            ToolTip.text: qsTr("Play a different file in this source")
            visible: insp.file
            text: qsTr("Choose a file…")
            icon.name: "film"
            onClicked: backend.replaceMedia(insp.s.sourceId)
        }
        ColumnLayout {
            visible: insp.file && !insp.playback.network
            Layout.fillWidth: true
            RowLayout {
                enabled: !insp.playback.closed && (insp.set.input ?? "") !== ""
                Button {
                    ToolTip.text: qsTr("Play or pause the file")
                    objectName: "mediaPlayPause"
                    text: insp.playback.paused || insp.playback.state === "ended" ? qsTr("Play") : qsTr("Pause")
                    enabled: insp.playback.state !== undefined
                    onClicked: {
                        if (insp.playback.state === "ended") backend.controlMedia(insp.s.sourceId,"restart")
                        else backend.controlMedia(insp.s.sourceId,"pause",insp.playback.paused ? 0 : 1)
                    }
                }
                Button { objectName: "mediaRestart"; text: qsTr("Restart"); onClicked: backend.controlMedia(insp.s.sourceId,"restart"); ToolTip.text: qsTr("Play from the beginning") }
                Button { objectName: "mediaStop"; text: qsTr("Stop"); onClicked: backend.controlMedia(insp.s.sourceId,"stop"); ToolTip.text: qsTr("Stop and go back to the start") }
            }
            Slider {
                ToolTip.text: qsTr("Drag to move through the file")
                id: mediaSeek
                objectName: "mediaSeek"
                Layout.fillWidth: true
                from: 0; to: Math.max(.001,insp.playback.duration ?? 0)
                enabled: insp.playback.seekable === true && (insp.playback.duration ?? 0) > 0
                property real requested: 0
                onMoved: { requested = value; if (!pressed) backend.controlMedia(insp.s.sourceId,"seek",requested) }
                onPressedChanged: { if (pressed) requested = value; else backend.controlMedia(insp.s.sourceId,"seek",requested) }
                Accessible.name: qsTr("Playback position")
                Binding { target: mediaSeek; property: "value"; value: insp.playback.position ?? 0; when: !mediaSeek.pressed }
            }
            Label {
                Layout.fillWidth: true
                function stamp(seconds) { const n=Math.max(0,Math.floor(seconds ?? 0)); return Math.floor(n/60)+":"+(n%60).toString().padStart(2,"0") }
                text: stamp(mediaSeek.pressed ? mediaSeek.value : insp.playback.position) + " / " + stamp(insp.playback.duration)
                color: Theme.textMuted
                font.family: Theme.monoFamily
            }
            Pair {
                Label { text: qsTr("Speed"); color: Theme.textSecondary; Layout.preferredWidth: Theme.wInspectorLabel }
                ComboBox {
                    ToolTip.text: qsTr("Play faster or slower; the sound keeps its pitch")
                    objectName: "mediaSpeed"
                    Layout.fillWidth: true
                    model: [{label:"0.25×",value:.25},{label:"0.5×",value:.5},{label:"0.75×",value:.75},
                            {label:"1×",value:1},{label:"1.25×",value:1.25},{label:"1.5×",value:1.5},
                            {label:"2×",value:2},{label:"3×",value:3},{label:"4×",value:4}]
                    textRole: "label"; valueRole: "value"
                    currentIndex: Math.max(0,indexOfValue(insp.set.speed ?? 1))
                    onActivated: insp.setting("speed",currentValue)
                    Accessible.name: qsTr("Playback speed")
                }
            }
        }
        CheckBox { visible: insp.file; text: qsTr("Loop"); checked: insp.set.loop ?? false; onToggled: insp.setting("loop", checked); ToolTip.text: qsTr("Start again from the beginning when it reaches the end") }
        CheckBox { visible: insp.file; text: qsTr("Start again when it becomes visible"); checked: insp.set.restart ?? true
                   ToolTip.text: qsTr("Play from the beginning each time it comes back into view")
                   onToggled: insp.setting("restart", checked) }
        CheckBox { objectName: "mediaCloseInactive"; visible: insp.kind === "media"; text: qsTr("Close when not visible"); checked: insp.set.closeInactive ?? false
                   ToolTip.text: qsTr("Close the file or network listener while no live studio shows it, to save memory; it reopens from the start when shown")
                   onToggled: insp.setting("closeInactive", checked) }
        Pair {
            visible: insp.kind === "media"
            Label { text: qsTr("Volume"); color: Theme.textSecondary; Layout.preferredWidth: Theme.wInspectorLabel }
            Slider {
                ToolTip.text: qsTr("The source's own volume, before the mixer")
                Layout.fillWidth: true
                from: 0; to: 1.5
                value: insp.set.volume ?? 1
                onMoved: insp.setting("volume", value, true)
                onPressedChanged: if (!pressed) backend.endGesture()
                Accessible.name: qsTr("Volume")
            }
            Label { text: Math.round((insp.set.volume ?? 1) * 100) + "%"; color: Theme.textMuted; font.family: Theme.monoFamily }
        }

        // Browser: the page, its size and how it runs.
        Label {
            visible: insp.kind === "browser" && !backend.browserAvailable
            Layout.fillWidth: true
            text: qsTr("Browser sources need Qt WebEngine. Install the qt6-webengine package and start OmaBroadcast again.")
            color: Theme.warning
            wrapMode: Text.Wrap
        }
        CheckBox {
            id: browserLocal
            objectName: "browserLocal"
            visible: insp.kind === "browser"
            text: qsTr("Local file")
            checked: insp.set.local ?? false
            ToolTip.text: qsTr("Show an HTML file from this computer instead of a web address")
            onToggled: insp.setting("local", checked)
        }
        TextField {
            ToolTip.text: qsTr("The page to show: a web address, or a local page served on this computer")
            objectName: "browserUrl"
            visible: insp.kind === "browser" && !browserLocal.checked
            Layout.fillWidth: true
            placeholderText: "https://"
            text: insp.set.url ?? ""
            inputMethodHints: Qt.ImhUrlCharactersOnly
            onEditingFinished: if (text !== (insp.set.url ?? "")) insp.setting("url", text.trim())
            Accessible.name: qsTr("Web address")
        }
        RowLayout {
            visible: insp.kind === "browser" && browserLocal.checked
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: (insp.set.path ?? "") !== "" ? insp.set.path : qsTr("No file chosen")
                color: Theme.textSecondary
                elide: Text.ElideMiddle
            }
            Button {
                objectName: "browserChooseFile"
                text: qsTr("Choose…")
                ToolTip.text: qsTr("Choose the HTML file to show")
                onClicked: backend.chooseBrowserFile(insp.s.sourceId)
            }
        }
        Pair {
            visible: insp.kind === "browser"
            NumField { objectName: "browserWidth"; label: qsTr("W"); value: insp.set.width ?? 800; Layout.fillWidth: true
                       ToolTip.text: qsTr("The page's width in pixels, as a browser window would be")
                       onCommitted: (v) => insp.setting("width", Math.max(1, Math.min(8192, Math.round(v)))) }
            NumField { objectName: "browserHeight"; label: qsTr("H"); value: insp.set.height ?? 600; Layout.fillWidth: true
                       ToolTip.text: qsTr("The page's height in pixels")
                       onCommitted: (v) => insp.setting("height", Math.max(1, Math.min(8192, Math.round(v)))) }
        }
        NumField {
            objectName: "browserFps"
            visible: insp.kind === "browser"
            Layout.fillWidth: true
            label: qsTr("Frame rate"); suffix: "fps"; value: insp.set.fps ?? 30
            ToolTip.text: qsTr("Pictures a second at most; a page that does not change sends none")
            onCommitted: (v) => insp.setting("fps", Math.max(1, Math.min(120, Math.round(v))))
        }
        Label {
            visible: insp.kind === "browser"
            text: qsTr("Custom CSS")
            color: Theme.textSecondary
        }
        ScrollView {
            visible: insp.kind === "browser"
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.hControl * 3
            TextArea {
                ToolTip.text: qsTr("Style added to every page this source loads, after the page's own; it applies as you type")
                objectName: "browserCss"
                textFormat: TextEdit.PlainText
                text: insp.set.css ?? ""
                wrapMode: TextEdit.Wrap
                font.family: Theme.monoFamily
                onTextChanged: if (activeFocus && text !== (insp.set.css ?? "")) insp.setting("css", text, true)
                onActiveFocusChanged: if (!activeFocus) backend.endGesture()
                Accessible.name: qsTr("Custom CSS")
            }
        }
        CheckBox {
            objectName: "browserShutdownHidden"
            visible: insp.kind === "browser"
            text: qsTr("Shut down when not visible")
            checked: insp.set.shutdownHidden ?? false
            ToolTip.text: qsTr("Close the page while no live studio shows it, to save memory and processor time; it loads afresh when shown")
            onToggled: insp.setting("shutdownHidden", checked)
        }
        CheckBox {
            objectName: "browserRefreshOnActive"
            visible: insp.kind === "browser"
            text: qsTr("Reload when it becomes visible")
            checked: insp.set.refreshOnActive ?? false
            ToolTip.text: qsTr("Load the page again each time a live studio starts showing it, so an animation plays from its start")
            onToggled: insp.setting("refreshOnActive", checked)
        }
        Pair {
            visible: insp.kind === "browser"
            Label { text: qsTr("Volume"); color: Theme.textSecondary; Layout.preferredWidth: Theme.wInspectorLabel }
            Slider {
                ToolTip.text: qsTr("The page's own volume, before the mixer")
                Layout.fillWidth: true
                from: 0; to: 1.5
                value: insp.set.volume ?? 1
                onMoved: insp.setting("volume", value, true)
                onPressedChanged: if (!pressed) backend.endGesture()
                Accessible.name: qsTr("Volume")
            }
            Label { text: Math.round((insp.set.volume ?? 1) * 100) + "%"; color: Theme.textMuted; font.family: Theme.monoFamily }
        }
        RowLayout {
            visible: insp.kind === "browser" && backend.browserAvailable
            Layout.fillWidth: true
            Button {
                objectName: "browserReload"
                text: qsTr("Reload")
                icon.name: "refresh-cw"
                ToolTip.text: qsTr("Load the page again, fetching everything afresh")
                onClicked: backend.reloadBrowser(insp.s.sourceId)
            }
            Button {
                objectName: "browserInteract"
                text: qsTr("Interact…")
                icon.name: "mouse-pointer-click"
                ToolTip.text: qsTr("Open the page in a window to click, scroll and type in it, for example to sign in")
                onClicked: backend.openInteract(insp.s.sourceId)
            }
        }

        // Capture device: which mode it runs in.
        ComboBox {
            ToolTip.text: qsTr("Resolution, frame rate and format the device sends. Best available takes its largest picture at the canvas's frame rate, or else the most pixels a second. A mode at another frame rate sets the canvas, and with it recordings and streams, to that rate.")
            id: modeBox
            visible: insp.kind === "device"
            Layout.fillWidth: true
            readonly property var entries: [{ key: "", label: insp.s.bestMode ? qsTr("Best available (%1)").arg(insp.s.bestMode) : qsTr("Best available") }].concat(insp.s.modes ?? [])
            model: entries
            textRole: "label"
            valueRole: "key"
            currentIndex: Math.max(0, indexOfValue(insp.set.mode ?? ""))
            onActivated: insp.setting("mode", currentValue)
            Accessible.name: qsTr("Mode")
        }

        // Its picture changes at a rate the canvas does not fit evenly: say so, with the way out.
        Rectangle {
            objectName: "evenRateNote"
            visible: insp.kind === "device" && (insp.s.evenRate?.num ?? 0) > 0
            Layout.fillWidth: true
            implicitHeight: evenRate.implicitHeight + Theme.s2 * 2
            radius: Theme.rControl
            color: Theme.withAlpha(Theme.warning, 0.10)
            border.width: Theme.hairline
            border.color: Theme.withAlpha(Theme.warning, 0.45)
            ColumnLayout {
                id: evenRate
                anchors.fill: parent
                anchors.margins: Theme.s2
                spacing: Theme.s2
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Its picture changes %1 times a second, but the canvas runs at %2, so some pictures show for longer than others and movement judders.")
                              .arg(insp.s.evenRate?.pictures ?? 0).arg(Number((backend.fpsNum / Math.max(1, backend.fpsDen)).toFixed(2)))
                    color: Theme.textPrimary
                    wrapMode: Text.Wrap
                }
                Button {
                    objectName: "useEvenRate"
                    text: qsTr("Use %1 frames a second").arg(insp.s.evenRate?.label ?? "")
                    ToolTip.text: qsTr("Sets the canvas, and with it recordings and streams, to this rate, and the device to its mode at that rate")
                    onClicked: backend.useEvenRate(insp.s.sourceId)
                }
            }
        }

        Label {
            visible: insp.kind === "device" && (insp.s.modes ?? []).length === 0
            Layout.fillWidth: true
            text: qsTr("Not connected. Plug it in and it starts by itself.")
            color: Theme.warning
            wrapMode: Text.Wrap
        }

        // Capture
        Switch {
            ToolTip.text: qsTr("Include the mouse pointer in the capture")
            visible: insp.capture
            text: qsTr("Show the pointer")
            checked: insp.set.cursor ?? true
            onToggled: insp.setting("cursor", checked)
        }
        Button {
            ToolTip.text: qsTr("Pick another display or window through the desktop's share dialog")
            visible: insp.capture
            text: insp.kind === "window" ? qsTr("Choose window…") : qsTr("Choose display…")
            icon.name: insp.kind === "window" ? "app-window" : "monitor"
            onClicked: backend.reopenSource(insp.s.sourceId)
        }
        Button {
            ToolTip.text: qsTr("Choose a display or window directly, without the share dialog, where the desktop allows it")
            visible: insp.capture
            text: qsTr("Direct capture…")
            onClicked: backend.chooseDirectCapture(insp.s.sourceId)
        }

        // Colour
        ColourField {
            ToolTip.text: qsTr("The colour, as #rrggbb or #aarrggbb (with transparency)")
            visible: insp.kind === "colour" || insp.kind === "text"
            Layout.fillWidth: true
            value: insp.set.colour ?? (insp.kind === "text" ? "#ffffff" : "#000000")
            onCommitted: (v) => insp.setting("colour", v)
        }
        Pair {
            visible: insp.kind === "colour"
            NumField { label: qsTr("W"); value: insp.set.width ?? 0; Layout.fillWidth: true; onCommitted: (v) => insp.setting("width", Math.max(1, Math.round(v))); ToolTip.text: qsTr("Width in pixels") }
            NumField { label: qsTr("H"); value: insp.set.height ?? 0; Layout.fillWidth: true; onCommitted: (v) => insp.setting("height", Math.max(1, Math.round(v))); ToolTip.text: qsTr("Height in pixels") }
        }

        // Image
        Label {
            visible: insp.kind === "image"
            Layout.fillWidth: true
            text: insp.set.path ?? ""
            color: Theme.textSecondary
            elide: Text.ElideMiddle
        }
        Button {
            ToolTip.text: qsTr("Show a different image")
            visible: insp.kind === "image"
            text: qsTr("Choose a file…")
            icon.name: "image"
            onClicked: backend.replaceImage(insp.s.sourceId)
        }

        // Slideshow
        RowLayout {
            visible: insp.kind === "slideshow" && (insp.set.presentation ?? "") !== ""
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: (insp.set.presentation ?? "").split("/").pop()
                color: Theme.textSecondary
                elide: Text.ElideMiddle
            }
            Button {
                objectName: "reloadPresentation"
                text: qsTr("Load again")
                ToolTip.text: qsTr("Load the presentation again after changing it in OmaShow")
                onClicked: backend.reloadPresentation(insp.s.sourceId)
            }
        }
        Button {
            ToolTip.text: qsTr("Add, remove and order the slideshow's images and folders")
            objectName: "editSlideshowImages"
            visible: insp.kind === "slideshow" && (insp.set.presentation ?? "") === ""
            text: qsTr("Edit images…")
            icon.name: "images"
            onClicked: backend.editSlideshow(insp.s.sourceId)
        }
        ColumnLayout {
            visible: insp.kind === "slideshow"
            Layout.fillWidth: true
            spacing: Theme.s2
            readonly property var playback: backend.selectedSlideshow
            Label {
                objectName: "slideshowStatus"
                Layout.fillWidth: true
                text: (parent.playback.index ?? -1) >= 0
                    ? qsTr("Slide %1 of %2%3").arg((parent.playback.index ?? 0) + 1).arg(parent.playback.count)
                        .arg(parent.playback.ended ? qsTr(" · Finished") : parent.playback.paused ? qsTr(" · Paused") : "")
                    : (parent.playback.count ?? 0) > 0 ? qsTr("Loading images…") : qsTr("No readable images")
                color: Theme.textSecondary
            }
            RowLayout {
                Layout.fillWidth: true
                enabled: (parent.playback.count ?? 0) > 0
                Button { objectName: "slideshowPrevious"; text: qsTr("Previous"); Layout.fillWidth: true
                    ToolTip.text: qsTr("Show the previous image")
                    onClicked: backend.controlSlideshow(insp.s.sourceId, "previous") }
                Button { objectName: "slideshowNext"; text: qsTr("Next"); Layout.fillWidth: true
                    ToolTip.text: qsTr("Show the next image")
                    onClicked: backend.controlSlideshow(insp.s.sourceId, "next") }
            }
            RowLayout {
                Layout.fillWidth: true
                enabled: (parent.playback.count ?? 0) > 0
                Button { objectName: "slideshowPause"; text: backend.selectedSlideshow.paused ? qsTr("Play") : qsTr("Pause"); Layout.fillWidth: true
                    ToolTip.text: qsTr("Pause or play the slideshow")
                    onClicked: backend.controlSlideshow(insp.s.sourceId, "pause", backend.selectedSlideshow.paused ? 0 : 1) }
                Button { text: qsTr("Restart"); Layout.fillWidth: true; onClicked: backend.controlSlideshow(insp.s.sourceId, "restart"); ToolTip.text: qsTr("Back to the first image and play") }
                Button { text: qsTr("Stop"); Layout.fillWidth: true; onClicked: backend.controlSlideshow(insp.s.sourceId, "stop"); ToolTip.text: qsTr("Back to the first image and pause") }
            }
        }
        CheckBox {
            ToolTip.text: qsTr("Change slides only with Previous and Next")
            objectName: "slideshowManual"
            visible: insp.kind === "slideshow"
            text: qsTr("Advance slides manually")
            checked: insp.set.manual ?? false
            onToggled: insp.setting("manual", checked)
        }
        ComboBox {
            ToolTip.text: qsTr("How one image gives way to the next")
            objectName: "slideshowTransition"
            visible: insp.kind === "slideshow"
            Layout.fillWidth: true
            model: [qsTr("Cut"), qsTr("Fade"), qsTr("Swipe"), qsTr("Slide")]
            readonly property var values: ["cut", "fade", "swipe", "slide"]
            currentIndex: Math.max(0, values.indexOf(insp.set.transition ?? "fade"))
            onActivated: insp.setting("transition", values[currentIndex])
            Accessible.name: qsTr("Slideshow transition")
        }
        ComboBox {
            ToolTip.text: qsTr("What the slideshow does while no live studio shows it")
            objectName: "slideshowVisibility"
            visible: insp.kind === "slideshow"
            Layout.fillWidth: true
            model: [qsTr("When hidden: continue"), qsTr("When hidden: pause"), qsTr("When hidden: restart")]
            readonly property var values: ["continue", "pause", "restart"]
            currentIndex: Math.max(0, values.indexOf(insp.set.visibility ?? "continue"))
            onActivated: insp.setting("visibility", values[currentIndex])
            Accessible.name: qsTr("Slideshow behaviour when hidden")
        }
        CheckBox {
            ToolTip.text: qsTr("Fade out after the last image instead of staying on it")
            objectName: "slideshowHideWhenDone"
            visible: insp.kind === "slideshow"
            Layout.fillWidth: true
            text: qsTr("Hide after the last slide")
            enabled: !(insp.set.loop ?? true)
            checked: insp.set.hideWhenDone ?? false
            onToggled: insp.setting("hideWhenDone", checked)
        }
        ColumnLayout {
            visible: insp.kind === "slideshow"
            Layout.fillWidth: true
            spacing: Theme.s2
            NumField { label: qsTr("Interval"); labelWidth: Theme.wInspectorLabel + Theme.s2; value: insp.set.interval ?? 5; suffix: "s"; decimals: 1; Layout.fillWidth: true
                       ToolTip.text: qsTr("Seconds each image stays")
                       onCommitted: (v) => insp.setting("interval", Math.max(0.2, v)) }
            NumField { label: qsTr("Transition"); labelWidth: Theme.wInspectorLabel + Theme.s2; value: insp.set.fade ?? 700; suffix: "ms"; Layout.fillWidth: true
                       ToolTip.text: qsTr("Length of the transition in milliseconds")
                       enabled: (insp.set.transition ?? "fade") !== "cut"
                       onCommitted: (v) => insp.setting("fade", Math.max(0, Math.round(v))) }
            Label {
                Layout.fillWidth: true
                text: qsTr("The transition fits within the interval between slides.")
                color: Theme.textMuted
                wrapMode: Text.Wrap
            }
        }
        Pair {
            visible: insp.kind === "slideshow"
            NumField { label: qsTr("W"); value: insp.set.width ?? insp.s.sourceWidth ?? 0; Layout.fillWidth: true; onCommitted: (v) => insp.setting("width", Math.max(1, Math.round(v))); ToolTip.text: qsTr("Width of the slideshow's box; images fit inside it") }
            NumField { label: qsTr("H"); value: insp.set.height ?? insp.s.sourceHeight ?? 0; Layout.fillWidth: true; onCommitted: (v) => insp.setting("height", Math.max(1, Math.round(v))); ToolTip.text: qsTr("Height of the slideshow's box; images fit inside it") }
        }
        Pair {
            visible: insp.kind === "slideshow"
            CheckBox { text: qsTr("Loop"); checked: insp.set.loop ?? true; onToggled: insp.setting("loop", checked); ToolTip.text: qsTr("Start again after the last image") }
            CheckBox { text: qsTr("Shuffle"); checked: insp.set.shuffle ?? false; onToggled: insp.setting("shuffle", checked); ToolTip.text: qsTr("Play the images in a random order") }
        }

        CheckBox {
            ToolTip.text: qsTr("Free the image's memory while no live studio shows it; it reloads when shown")
            objectName: "imageUnloadInactive"
            visible: insp.kind === "image"
            text: qsTr("Unload when not visible")
            checked: insp.set.unloadInactive ?? false
            onToggled: insp.setting("unloadInactive", checked)
        }

        CheckBox {
            objectName: "sourceLinearAlpha"
            Layout.fillWidth: true
            visible: insp.kind === "image" || insp.kind === "media"
            text: qsTr("Apply alpha in linear light")
            checked: insp.set.linearAlpha ?? true
            onToggled: insp.setting("linearAlpha", checked)
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Keep on for linear-light transparency. Turn off for artwork that expects alpha to be applied to encoded colours. This changes every use of the source without restarting playback.")
        }

        // Text
        CheckBox {
            ToolTip.text: qsTr("Use HTML markup for mixed fonts, colours, lists and emoji; off shows the text exactly as typed")
            objectName: "richTextToggle"
            visible: insp.kind === "text"
            text: qsTr("Rich text (HTML)")
            checked: insp.set.richText ?? false
            onToggled: insp.setting("richText", checked)
        }
        Label {
            visible: insp.kind === "text" && (insp.set.richText ?? false)
            Layout.fillWidth: true
            text: qsTr("Use HTML for mixed fonts, colours, headings and lists. Gradient overrides the text colours.")
            color: Theme.textMuted
            wrapMode: Text.Wrap
        }
        Pair {
            visible: insp.kind === "text"
            CheckBox {
                ToolTip.text: qsTr("Show the text of a file, following it as it changes")
                id: fromFile
                objectName: "textFromFile"
                text: qsTr("From a file")
                checked: (insp.set.file ?? "") !== ""
                // Ticked by a file being chosen, not by asking for one: a cancelled picker changes nothing.
                onToggled: {
                    if (checked) backend.chooseTextFile(insp.s.sourceId)
                    else insp.setting("file", "")
                    checked = Qt.binding(() => (insp.set.file ?? "") !== "")
                }
            }
            Item { Layout.fillWidth: true }
            NumField { visible: fromFile.checked; label: qsTr("Last"); value: insp.set.lines ?? 0; suffix: qsTr("lines")
                       ToolTip.text: qsTr("Show only the last lines of the file; 0 shows it all")
                       Layout.preferredWidth: Theme.s6 * 4
                       onCommitted: (v) => insp.setting("lines", Math.max(0, Math.round(v))) }
        }
        RowLayout {
            visible: insp.kind === "text" && fromFile.checked
            Layout.fillWidth: true
            Label { Layout.fillWidth: true; text: insp.set.file ?? ""; color: Theme.textSecondary; elide: Text.ElideMiddle }
            Button { text: qsTr("Change…"); onClicked: backend.chooseTextFile(insp.s.sourceId); ToolTip.text: qsTr("Choose a different text file") }
        }
        ScrollView {
            visible: insp.kind === "text" && !fromFile.checked
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.hControl * 3
            TextArea {
                ToolTip.text: qsTr("The text to show")
                objectName: "sourceTextEditor"
                textFormat: TextEdit.PlainText
                text: insp.set.text ?? ""
                wrapMode: TextEdit.Wrap
                // Typing is one undo step per stretch of typing.
                onTextChanged: if (activeFocus && text !== (insp.set.text ?? "")) insp.setting("text", text, true)
                onActiveFocusChanged: if (!activeFocus) backend.endGesture()
                Accessible.name: qsTr("Text")
            }
        }
        TextField {
            ToolTip.text: qsTr("Font name, as installed; empty uses the default")
            visible: insp.kind === "text"
            Layout.fillWidth: true
            placeholderText: qsTr("Font family")
            text: insp.set.font ?? ""
            onEditingFinished: if (text !== (insp.set.font ?? "")) insp.setting("font", text)
        }
        Pair {
            visible: insp.kind === "text"
            NumField { label: qsTr("Size"); value: insp.set.size ?? 96; Layout.fillWidth: true; onCommitted: (v) => insp.setting("size", Math.max(4, Math.round(v))); ToolTip.text: qsTr("Font size in pixels") }
            NumField { label: qsTr("Wrap"); value: insp.set.wrapWidth ?? 0; suffix: "px"; Layout.fillWidth: true
                       ToolTip.text: qsTr("Wrap lines at this width in pixels; 0 never wraps")
                       onCommitted: (v) => insp.setting("wrapWidth", Math.max(0, Math.round(v))) }
        }
        Pair {
            visible: insp.kind === "text"
            CheckBox { text: qsTr("Bold"); checked: insp.set.bold ?? false; onToggled: insp.setting("bold", checked); ToolTip.text: qsTr("Bold text") }
            CheckBox { text: qsTr("Italic"); checked: insp.set.italic ?? false; onToggled: insp.setting("italic", checked); ToolTip.text: qsTr("Italic text") }
            ComboBox {
                ToolTip.text: qsTr("Line alignment")
                Layout.fillWidth: true
                model: [qsTr("Left"), qsTr("Centre"), qsTr("Right")]
                readonly property var values: ["left", "centre", "right"]
                currentIndex: Math.max(0, values.indexOf(insp.set.align ?? "left"))
                onActivated: insp.setting("align", values[currentIndex])
            }
        }
        Label { visible: insp.kind === "text"; text: qsTr("Background"); color: Theme.textSecondary; font.pixelSize: Theme.fsLabel }
        ColourField {
            ToolTip.text: qsTr("A box behind the text; #00000000 is none")
            visible: insp.kind === "text"
            Layout.fillWidth: true
            value: insp.set.background ?? "#00000000"
            onCommitted: (v) => insp.setting("background", v)
        }
    }

    InspectorSection {
        objectName: "effectsSection"
        visible: insp.kind === "text"
        title: qsTr("Text effects")
        key: "inspector.effects"
        startOpen: false
        // Outline, shadow, gradient
        CheckBox {
            ToolTip.text: qsTr("Antialiased edges; off gives crisp pixel edges for small or pixel-art text")
            visible: insp.kind === "text"
            text: qsTr("Smooth text edges")
            checked: insp.set.antialias ?? true
            onToggled: insp.setting("antialias", checked)
        }
        Pair {
            visible: insp.kind === "text"
            Label { text: qsTr("Outline"); color: Theme.textSecondary; Layout.fillWidth: true }
            NumField { value: insp.set.outlineWidth ?? 0; suffix: "px"; Layout.preferredWidth: Theme.s6 * 3
                       ToolTip.text: qsTr("Outline thickness in pixels; 0 is none")
                       onCommitted: (v) => insp.setting("outlineWidth", Math.max(0, v)) }
        }
        ColourField {
            ToolTip.text: qsTr("Outline colour")
            visible: insp.kind === "text" && (insp.set.outlineWidth ?? 0) > 0
            Layout.fillWidth: true
            value: insp.set.outlineColour ?? "#000000"
            onCommitted: (v) => insp.setting("outlineColour", v)
        }
        CheckBox { visible: insp.kind === "text"; text: qsTr("Shadow"); checked: insp.set.shadow ?? false
                   ToolTip.text: qsTr("A soft shadow behind the text")
                   onToggled: insp.setting("shadow", checked) }
        ColourField {
            ToolTip.text: qsTr("Shadow colour, with transparency")
            visible: insp.kind === "text" && (insp.set.shadow ?? false)
            Layout.fillWidth: true
            value: insp.set.shadowColour ?? "#b0000000"
            onCommitted: (v) => insp.setting("shadowColour", v)
        }
        Pair {
            visible: insp.kind === "text" && (insp.set.shadow ?? false)
            NumField { label: qsTr("X"); value: insp.set.shadowX ?? Math.round((insp.set.size ?? 96) / 16); Layout.fillWidth: true
                       ToolTip.text: qsTr("Shadow offset to the right, in pixels")
                       onCommitted: (v) => insp.setting("shadowX", Math.round(v)) }
            NumField { label: qsTr("Y"); value: insp.set.shadowY ?? Math.round((insp.set.size ?? 96) / 16); Layout.fillWidth: true
                       ToolTip.text: qsTr("Shadow offset downwards, in pixels")
                       onCommitted: (v) => insp.setting("shadowY", Math.round(v)) }
            NumField { label: qsTr("Blur"); value: insp.set.shadowBlur ?? Math.round((insp.set.size ?? 96) / 24); Layout.fillWidth: true
                       ToolTip.text: qsTr("Shadow softness in pixels")
                       onCommitted: (v) => insp.setting("shadowBlur", Math.max(0, Math.round(v))) }
        }
        CheckBox { visible: insp.kind === "text"; text: qsTr("Gradient"); checked: insp.set.gradient ?? false
                   ToolTip.text: qsTr("Fade the text's colour into a second colour")
                   onToggled: insp.setting("gradient", checked) }
        Pair {
            visible: insp.kind === "text" && (insp.set.gradient ?? false)
            ColourField { Layout.fillWidth: true; value: insp.set.gradientColour ?? "#ff8c00"
                          ToolTip.text: qsTr("The second colour of the gradient")
                          onCommitted: (v) => insp.setting("gradientColour", v) }
            NumField { value: insp.set.gradientAngle ?? 90; suffix: "°"; Layout.preferredWidth: Theme.s6 * 3
                       ToolTip.text: qsTr("Gradient direction in degrees")
                       onCommitted: (v) => insp.setting("gradientAngle", ((Math.round(v) % 360) + 360) % 360) }
        }
    }

    InspectorSection {
        objectName: "filtersSection"
        visible: !insp.audioOnly && (insp.s.sourceId ?? "") !== ""
        title: qsTr("Filters")
        key: "inspector.filters"
        startOpen: false
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            text: (insp.set.videoFilters ?? []).length === 0 ? qsTr("No filters")
                : (insp.set.videoFilters ?? []).map(f => (backend.videoFilterTypes.find(d => d.type === f.type)?.label ?? f.type) + (f.enabled === false ? qsTr(" (off)") : "")).join(", ")
        }
        Pair {
            Button {
                objectName: "editVideoFilters"
                ToolTip.text: qsTr("Add, change and reorder this source's filters")
                Layout.fillWidth: true
                text: qsTr("Edit filters…")
                onClicked: backend.editVideoFilters(insp.s.sourceId)
            }
            Button {
                ToolTip.text: qsTr("Copy these filters, to paste onto another source")
                flat: true
                text: qsTr("Copy")
                enabled: (insp.set.videoFilters ?? []).length > 0
                onClicked: backend.copyVideoFilters(insp.s.sourceId)
            }
            Button {
                ToolTip.text: qsTr("Add the copied filters after this source's own")
                flat: true
                text: qsTr("Paste")
                enabled: backend.canPasteVideoFilters
                onClicked: backend.pasteVideoFilters(insp.s.sourceId)
            }
        }
    }

    InspectorSection {
        objectName: "pictureSection"
        visible: insp.capture || insp.kind === "device" || insp.kind === "media"
        title: qsTr("Picture")
        key: "inspector.picture"
        startOpen: false
        // The device's own picture controls, whatever it has.
        RowLayout {
            visible: insp.kind === "device" && (insp.s.controls ?? []).length > 0
            Layout.fillWidth: true
            Layout.topMargin: Theme.s2
            Label { text: qsTr("Picture"); color: Theme.textSecondary; Layout.fillWidth: true }
            Button {
                ToolTip.text: qsTr("Put the device's picture controls back to its own defaults")
                flat: true
                text: qsTr("Defaults")
                onClicked: backend.resetDeviceControls(insp.s.sourceId)
            }
        }
        Repeater {
            model: insp.controlIds
            delegate: ColumnLayout {
                required property var modelData
                readonly property var control: insp.controlOf(modelData)
                Layout.fillWidth: true
                spacing: 0
                opacity: control.inactive ? Theme.disabledOpacity : 1
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: control.name ?? ""; color: Theme.textSecondary; font.pixelSize: Theme.fsLabel; Layout.fillWidth: true; elide: Text.ElideRight }
                    Label {
                        visible: control.type === "int"
                        text: control.value ?? ""
                        color: Theme.textMuted
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsLabel
                    }
                    Switch {
                        ToolTip.text: control.name ?? ""
                        visible: control.type === "bool"
                        checked: control.value !== 0
                        onToggled: backend.setDeviceControl(insp.s.sourceId, modelData, checked ? 1 : 0)
                        Accessible.name: control.name ?? ""
                    }
                }
                Slider {
                    objectName: "deviceSlider-" + modelData
                    ToolTip.text: control.name ?? ""
                    visible: control.type === "int"
                    Layout.fillWidth: true
                    enabled: !control.inactive
                    from: control.min ?? 0; to: control.max ?? 0; stepSize: control.step ?? 1
                    value: control.value ?? 0
                    onMoved: backend.setDeviceControl(insp.s.sourceId, modelData, Math.round(value), true)
                    onPressedChanged: if (!pressed) backend.endGesture()
                    Accessible.name: control.name ?? ""
                }
                ComboBox {
                    objectName: "deviceMenu-" + modelData
                    ToolTip.text: control.name ?? ""
                    visible: control.type === "menu"
                    Layout.fillWidth: true
                    model: control.menu ?? []
                    textRole: "label"
                    valueRole: "value"
                    // Only once it is made: until then indexOfValue finds nothing.
                    Component.onCompleted: currentIndex = Qt.binding(() => Math.max(0, indexOfValue(control.value)))
                    onActivated: backend.setDeviceControl(insp.s.sourceId, modelData, currentValue)
                    Accessible.name: control.name ?? ""
                }
            }
        }

        // Noise reduction, for anything captured or played.
        Pair {
            visible: insp.capture || insp.kind === "device" || insp.kind === "media"
            Label { text: qsTr("Noise reduction"); color: Theme.textSecondary; Layout.fillWidth: true }
            ComboBox {
                ToolTip.text: qsTr("Smooth out camera grain and compression noise. Higher is smoother and softer")
                Layout.preferredWidth: Theme.s6 * 3
                model: [qsTr("Off"), qsTr("Low"), qsTr("Medium"), qsTr("High")]
                currentIndex: Math.max(0, Math.min(3, insp.set.denoise ?? 0))
                onActivated: insp.setting("denoise", currentIndex)
                Accessible.name: qsTr("Noise reduction")
            }
        }

        // Colour: for streams that label themselves wrongly.
        Label {
            visible: insp.kind === "device" || insp.kind === "media"
            text: qsTr("Colour")
            color: Theme.textSecondary
            Layout.topMargin: Theme.s2
        }
        Pair {
            visible: insp.kind === "device" || insp.kind === "media"
            ComboBox {
                ToolTip.text: qsTr("Auto detects it; override when blacks look grey or highlights clip. Many webcams wrongly label full range as limited")
                Layout.fillWidth: true
                model: [qsTr("Range: auto"), qsTr("Limited (16–235)"), qsTr("Full (0–255)")]
                readonly property var values: ["auto", "limited", "full"]
                currentIndex: Math.max(0, values.indexOf(insp.set.range ?? "auto"))
                onActivated: insp.setting("range", values[currentIndex])
                Accessible.name: qsTr("Colour range")
            }
            ComboBox {
                ToolTip.text: qsTr("Which colour conversion the source uses; auto follows its label or its size")
                Layout.fillWidth: true
                model: [qsTr("Matrix: auto"), "BT.601", "BT.709", "BT.2020"]
                readonly property var values: ["auto", "bt601", "bt709", "bt2020"]
                currentIndex: Math.max(0, values.indexOf(insp.set.matrix ?? "auto"))
                onActivated: insp.setting("matrix", values[currentIndex])
                Accessible.name: qsTr("Colour matrix")
            }
        }
    }

    InspectorSection {
        objectName: "regionSection"
        visible: insp.kind === "region"
        title: qsTr("Display region")
        key: "inspector.region"
        Label {
            visible: (insp.s.captureWidth ?? 0) > 0
            Layout.fillWidth: true
            text: qsTr("Display %1 × %2 · Region %3 × %4").arg(insp.s.captureWidth ?? 0).arg(insp.s.captureHeight ?? 0)
                .arg(insp.s.sourceWidth ?? 0).arg(insp.s.sourceHeight ?? 0)
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }
        Pair {
            NumField {
                ToolTip.text: qsTr("Left edge of the region, in display pixels")
                objectName: "captureRegionX"
                label: qsTr("X"); value: insp.set.regionX ?? 0; Layout.fillWidth: true
                onCommitted: (v) => backend.setCaptureRegion(insp.s.sourceId, v, insp.set.regionY ?? 0, insp.set.regionWidth ?? 0, insp.set.regionHeight ?? 0)
            }
            NumField {
                ToolTip.text: qsTr("Top edge of the region, in display pixels")
                objectName: "captureRegionY"
                label: qsTr("Y"); value: insp.set.regionY ?? 0; Layout.fillWidth: true
                onCommitted: (v) => backend.setCaptureRegion(insp.s.sourceId, insp.set.regionX ?? 0, v, insp.set.regionWidth ?? 0, insp.set.regionHeight ?? 0)
            }
        }
        Pair {
            NumField {
                ToolTip.text: qsTr("Region width in display pixels; 0 runs to the right edge")
                objectName: "captureRegionWidth"
                label: qsTr("W"); value: insp.set.regionWidth ?? 0; Layout.fillWidth: true
                onCommitted: (v) => backend.setCaptureRegion(insp.s.sourceId, insp.set.regionX ?? 0, insp.set.regionY ?? 0, v, insp.set.regionHeight ?? 0)
            }
            NumField {
                ToolTip.text: qsTr("Region height in display pixels; 0 runs to the bottom edge")
                objectName: "captureRegionHeight"
                label: qsTr("H"); value: insp.set.regionHeight ?? 0; Layout.fillWidth: true
                onCommitted: (v) => backend.setCaptureRegion(insp.s.sourceId, insp.set.regionX ?? 0, insp.set.regionY ?? 0, insp.set.regionWidth ?? 0, v)
            }
        }
        Button {
            ToolTip.text: qsTr("Drag a rectangle on the display itself; OmaBroadcast steps aside while you draw, and Esc cancels")
            objectName: "drawCaptureRegion"
            text: qsTr("Draw region…")
            enabled: (insp.s.captureWidth ?? 0) > 0
            onClicked: backend.drawCaptureRegion(insp.s.sourceId)
        }
        Button {
            ToolTip.text: qsTr("Capture the whole display again")
            objectName: "resetCaptureRegion"
            text: qsTr("Use full display")
            onClicked: backend.setCaptureRegion(insp.s.sourceId, 0, 0, 0, 0)
        }
    }

    InspectorSection {
        objectName: "deinterlaceSection"
        visible: insp.capture || insp.kind === "device" || insp.kind === "media"
        title: qsTr("Deinterlacing")
        key: "inspector.deinterlace"
        startOpen: false
        ComboBox {
            ToolTip.text: qsTr("For interlaced video (old cameras, TV capture). 2× modes show both fields: use 50 fps or more for 25 fps input, 60 for 30")
            objectName: "deinterlaceMode"
            Layout.fillWidth: true
            readonly property var values: ["off", "discard", "discard2x", "blend", "blend2x", "linear", "linear2x", "adaptive", "adaptive2x"]
            model: [qsTr("Off"), qsTr("Discard field"), qsTr("Discard field · 2×"), qsTr("Blend"), qsTr("Blend · 2×"),
                    qsTr("Linear"), qsTr("Linear · 2×"), qsTr("Motion adaptive"), qsTr("Motion adaptive · 2×")]
            currentIndex: Math.max(0, values.indexOf(insp.set.deinterlace ?? "off"))
            onActivated: insp.setting("deinterlace", values[currentIndex])
            Accessible.name: qsTr("Deinterlacing mode")
        }
        ComboBox {
            ToolTip.text: qsTr("Which field comes first; automatic reads it from the input, or assumes top first")
            objectName: "deinterlaceOrder"
            Layout.fillWidth: true
            enabled: (insp.set.deinterlace ?? "off") !== "off"
            readonly property var values: ["auto", "top", "bottom"]
            model: [qsTr("Field order: Automatic"), qsTr("Top field first"), qsTr("Bottom field first")]
            currentIndex: Math.max(0, values.indexOf(insp.set.fieldOrder ?? "auto"))
            onActivated: insp.setting("fieldOrder", values[currentIndex])
            Accessible.name: qsTr("Field order")
        }
    }

    // How it appears and goes when shown or hidden.
    InspectorSection {
        id: showHide
        objectName: "showHideSection"
        visible: !insp.audioOnly && (insp.s.id ?? "") !== ""
        title: qsTr("Show and hide")
        key: "inspector.showHide"
        startOpen: false
        readonly property var choices: [{ id: "", name: qsTr("At once") }].concat(backend.transitions.filter(t => t.id !== "cut"))
        Repeater {
            model: [true, false]
            Pair {
                id: row
                required property bool modelData
                readonly property string chosen: (row.modelData ? insp.s.showTransition : insp.s.hideTransition) ?? ""
                readonly property int ms: (row.modelData ? insp.s.showMs : insp.s.hideMs) ?? 300
                Label { text: row.modelData ? qsTr("Show") : qsTr("Hide"); color: Theme.textSecondary; Layout.preferredWidth: Theme.wInspectorLabel }
                ComboBox {
                    objectName: row.modelData ? "itemShowTransition" : "itemHideTransition"
                    Layout.fillWidth: true
                    model: showHide.choices
                    textRole: "name"
                    currentIndex: Math.max(0, model.findIndex(c => c.id === row.chosen))
                    onActivated: backend.setItemTransition(insp.s.id, row.modelData, model[currentIndex].id, row.ms)
                    Accessible.name: row.modelData ? qsTr("Show transition") : qsTr("Hide transition")
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: row.modelData ? qsTr("How the source appears when it is shown") : qsTr("How the source goes when it is hidden")
                }
                NumField {
                    objectName: row.modelData ? "itemShowMs" : "itemHideMs"
                    Layout.preferredWidth: Theme.s6 * 2
                    enabled: row.chosen !== ""
                    value: row.ms
                    suffix: " ms"
                    onCommitted: (v) => backend.setItemTransition(insp.s.id, row.modelData, row.chosen, Math.max(0, Math.min(20000, Math.round(v))))
                    ToolTip.text: qsTr("How long it takes, in milliseconds")
                }
            }
        }
    }

    TransformControls {
        Layout.fillWidth: true
        visible: !insp.audioOnly
        onEditRequested: (id) => backend.editTransform(id)
    }
}