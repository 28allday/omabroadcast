import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// One place a stream goes: the service (or a server of your own), its key,
// and how it is sent. The key goes to the keyring, never into the profile,
// and is only shown again by asking for it to be replaced.
Sheet {
    id: sheet
    helpTopic: "settings-streaming"
    objectName: "streamSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, Theme.wSheet)
    height: Math.min(parent.height - Theme.s5 * 2, implicitHeight)
    modal: true
    standardButtons: Dialog.Cancel | Dialog.Save

    property var target: ({})
    property bool hadKey: false
    property bool keyEdited: false
    property bool passwordEdited: false
    readonly property bool srt: custom && /^srt:/i.test(serverField.text.trim())
    readonly property var service: backend.streamServices.find(s => s.id === serviceBox.currentValue) ?? backend.streamServices[0]
    readonly property bool custom: serviceBox.currentValue === "custom"
    readonly property bool needsServer: custom && serverField.text.trim() === ""
    // Why the last save was refused, shown over the form brought back for it.
    property string refusal: ""
    property bool saving: false
    // The target as it stands in the form, for the encoder list and warnings.
    readonly property var draft: ({
        id: target.id ?? "", name: nameField.text.trim(), service: serviceBox.currentValue,
        server: custom ? serverField.text.trim() : (serverBox.currentValue ?? ""),
        encoder: encoderBox.currentValue ?? "", bitrateKbps: Math.round(bitrateField.value),
        audioBitrateKbps: Math.round(audioField.value), track: trackBox.currentIndex,
        keyframeSeconds: Math.round(keyframeField.value), delaySeconds: Math.round(delayField.value),
        reconnect: reconnectCheck.checked, retryDelaySeconds: Math.round(retryField.value),
        maxRetries: Math.round(retriesField.value), dynamicBitrate: dynamicCheck.checked,
        bindAddress: bindField.text.trim(), enabled: target.enabled ?? true,
        preserveDelay: preserveCheck.checked, ipFamily: familyBox.currentValue ?? 0, lowLatency: lowLatencyCheck.checked,
        username: custom ? userField.text.trim() : "", enforce: enforceCheck.checked,
        outputHeight: sizeBox.currentValue ?? 0,
        muxer: custom ? muxerField.text.trim() : "", muxerOptions: muxerOptionsField.text.trim()
    })

    function edit(existing) {
        target = existing
        refusal = ""
        title = existing.id ? qsTr("Edit stream") : qsTr("Add a stream")
        serviceBox.currentIndex = Math.max(0, serviceBox.indexOfValue(existing.service ?? "twitch"))
        nameField.text = existing.name ?? ""
        const servers = sheet.service.servers
        serverField.text = existing.server ?? ""
        serverBox.currentIndex = Math.max(0, servers.findIndex(s => s.url === existing.server))
        keyField.text = ""
        keyEdited = false
        hadKey = existing.id ? backend.streamTargetHasKey(existing.id) : false
        bitrateField.value = existing.bitrateKbps ?? 6000
        audioField.value = existing.audioBitrateKbps ?? 160
        trackBox.currentIndex = existing.track ?? 0
        keyframeField.value = existing.keyframeSeconds ?? 2
        delayField.value = existing.delaySeconds ?? 0
        reconnectCheck.checked = existing.reconnect ?? true
        retryField.value = existing.retryDelaySeconds ?? 2
        retriesField.value = existing.maxRetries ?? 20
        dynamicCheck.checked = existing.dynamicBitrate ?? false
        bindField.text = existing.bindAddress ?? ""
        preserveCheck.checked = existing.preserveDelay ?? false
        familyBox.currentIndex = Math.max(0, familyBox.indexOfValue(existing.ipFamily ?? 0))
        lowLatencyCheck.checked = existing.lowLatency ?? false
        userField.text = existing.username ?? ""
        passwordField.text = ""
        passwordEdited = false
        enforceCheck.checked = existing.enforce ?? true
        sizeBox.currentIndex = Math.max(0, sizeBox.indexOfValue(existing.outputHeight ?? 0))
        muxerField.text = existing.muxer ?? ""
        muxerOptionsField.text = existing.muxerOptions ?? ""
        open()
        encoderBox.chosen = existing.encoder ?? ""
        encoderBox.currentIndex = Math.max(0, encoderBox.indexOfValue(encoderBox.chosen))
    }

    Component.onCompleted: standardButton(Dialog.Save).enabled = Qt.binding(() => !sheet.needsServer)
    Connections { target: backend; function onFailed(message) { if (sheet.saving) sheet.refusal = message } }
    // The sheet is on its way out by now. A save refused opens it again as it was,
    // once it has gone, so what was typed, the key above all, is not lost.
    property bool bringBack: false
    onAccepted: {
        refusal = ""
        saving = true
        const saved = backend.saveStreamTarget(passwordEdited ? Object.assign({ password: passwordField.text }, draft) : draft,
                                               keyField.text.trim(), keyEdited)
        saving = false
        if (saved !== "") return
        scroll.contentItem.contentY = 0
        if (visible) bringBack = true
        else open()
    }
    onClosed: if (bringBack) { bringBack = false; open() }

    component Fact: Label {
        color: Theme.textSecondary
        Layout.preferredWidth: Theme.wFactLabel
    }
    component Note: Label {
        Layout.fillWidth: true
        color: Theme.textMuted
        font.pixelSize: Theme.fsLabel
        wrapMode: Text.Wrap
    }

    contentItem: ScrollView {
        id: scroll
        clip: true
        contentWidth: availableWidth
        implicitHeight: grid.implicitHeight
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        GridLayout {
            id: grid
            width: scroll.availableWidth - Theme.s3
            columns: 2
            columnSpacing: Theme.s4
            rowSpacing: Theme.s2

            Label {
                objectName: "streamRefusal"
                visible: sheet.refusal !== ""
                Layout.columnSpan: 2
                Layout.fillWidth: true
                text: sheet.refusal
                color: Theme.danger
                wrapMode: Text.Wrap
            }

            Fact { text: qsTr("Service") }
            ComboBox {
                ToolTip.text: qsTr("Where you go live. Custom server takes any address of your own")
                id: serviceBox
                objectName: "streamService"
                Layout.fillWidth: true
                model: backend.streamServices
                textRole: "name"
                valueRole: "id"
            }

            Fact { text: qsTr("Server") }
            ColumnLayout {
                Layout.fillWidth: true
                ComboBox {
                    ToolTip.text: qsTr("Which of the service's servers to send to; encrypted (RTMPS) where offered")
                    id: serverBox
                    visible: !sheet.custom
                    Layout.fillWidth: true
                    model: sheet.service.servers
                    textRole: "name"
                    valueRole: "url"
                }
                TextField {
                    ToolTip.text: qsTr("RTMP and RTMPS take the key after the address; SRT takes it as its stream ID; an https:// address is WHIP (WebRTC), whose key is its bearer token, or HLS when it names a .m3u8. Put {key} in the address to place the key yourself")
                    id: serverField
                    objectName: "streamServer"
                    visible: sheet.custom
                    Layout.fillWidth: true
                    placeholderText: muxerField.text.trim() !== "" ? qsTr("Any address FFmpeg writes to: udp://, tcp://, rtp://…")
                                                                   : "rtmp://, rtmps://, srt:// or https://"
                    font.family: Theme.monoFamily
                }
                Note {
                    objectName: "streamNeedsServer"
                    visible: sheet.needsServer
                    text: qsTr("A custom stream needs a server address.")
                }
            }

            Fact { visible: sheet.custom; text: qsTr("Container") }
            ColumnLayout {
                visible: sheet.custom
                Layout.fillWidth: true
                TextField {
                    ToolTip.text: qsTr("Send in any container FFmpeg can write, by its own name (ffmpeg -muxers lists them), to any address it writes to. Empty picks the one the address implies")
                    id: muxerField
                    objectName: "streamMuxer"
                    Layout.fillWidth: true
                    placeholderText: qsTr("From the address (or an FFmpeg container: mpegts, nut…)")
                    font.family: Theme.monoFamily
                }
                Note {
                    objectName: "streamMuxerNote"
                    readonly property var info: backend.muxerInfo(muxerField.text)
                    visible: muxerField.text.trim() !== ""
                    text: info.problem !== "" ? info.problem : qsTr("%1, sent as it is to the address.").arg(info.label ?? "")
                }
            }

            Fact { text: sheet.service.keyLabel ?? qsTr("Stream key") }
            ColumnLayout {
                Layout.fillWidth: true
                TextField {
                    ToolTip.text: qsTr("The key from the service's dashboard. Kept in your keyring and never shown again; type a new one to replace it, or leave it to keep the saved one")
                    id: keyField
                    objectName: "streamKey"
                    Layout.fillWidth: true
                    echoMode: showKey.checked ? TextInput.Normal : TextInput.Password
                    placeholderText: sheet.hadKey ? qsTr("Saved — type to replace it") : qsTr("From the service's dashboard")
                    onTextEdited: sheet.keyEdited = true
                }
                CheckBox { id: showKey; text: qsTr("Show while typing"); ToolTip.text: qsTr("Show the key as you type it") }
                Note {
                    text: backend.keyringAvailable ? qsTr("Kept in your keyring, not in the profile.")
                                                   : qsTr("No keyring is running: kept in the profile file.")
                }
            }

            Fact { text: sheet.srt ? qsTr("Passphrase") : qsTr("Sign in"); visible: sheet.custom }
            ColumnLayout {
                visible: sheet.custom
                Layout.fillWidth: true
                RowLayout {
                    Layout.fillWidth: true
                    TextField {
                        id: userField
                        objectName: "streamUser"
                        visible: !sheet.srt
                        Layout.fillWidth: true
                        placeholderText: qsTr("Name, if the server asks")
                        Accessible.name: qsTr("Sign-in name")
                        ToolTip.text: qsTr("For a server of your own that wants a name and password (RTMP or HTTP). Leave empty when it does not")
                    }
                    TextField {
                        id: passwordField
                        objectName: "streamPassword"
                        Layout.fillWidth: true
                        echoMode: TextInput.Password
                        placeholderText: (sheet.target.hasPassword ?? false) ? qsTr("Saved — type to replace it")
                                       : sheet.srt ? qsTr("10 to 79 characters, if the listener has one") : qsTr("Password")
                        onTextEdited: sheet.passwordEdited = true
                        Accessible.name: sheet.srt ? qsTr("SRT passphrase") : qsTr("Sign-in password")
                        ToolTip.text: qsTr("Kept in your keyring with the stream key")
                    }
                }
            }

            Fact { text: qsTr("Name") }
            TextField {
                ToolTip.text: qsTr("A name for this stream in Settings and the status line; empty uses the service's name")
                id: nameField
                Layout.fillWidth: true
                placeholderText: sheet.service.name ?? ""
            }

            Fact { text: qsTr("Encoder") }
            ComboBox {
                ToolTip.text: qsTr("What encodes this stream. Best available picks what the service and protocol take, preferring hardware")
                id: encoderBox
                objectName: "streamEncoder"
                Layout.fillWidth: true
                // From where it goes only: the choice itself is not an input.
                model: backend.streamEncoderChoices({ service: serviceBox.currentValue,
                                                      server: sheet.custom ? serverField.text.trim() : (serverBox.currentValue ?? "") })
                textRole: "label"
                valueRole: "name"
                // The list is made again as the server is typed: the choice stays,
                // where the new list has it.
                property string chosen: ""
                onActivated: chosen = currentValue ?? ""
                onModelChanged: currentIndex = Math.max(0, indexOfValue(chosen))
            }

            Fact { text: qsTr("Size") }
            ComboBox {
                ToolTip.text: qsTr("The picture size this stream goes out at, in the canvas's shape. Smaller than the canvas saves bandwidth; the services' warnings below follow it")
                id: sizeBox
                objectName: "streamSize"
                Layout.fillWidth: true
                model: backend.outputSizes
                textRole: "label"
                valueRole: "height"
            }

            Fact { text: qsTr("Bitrate") }
            RowLayout {
                Layout.fillWidth: true
                NumField { id: bitrateField; objectName: "streamBitrate"; suffix: qsTr("kb/s"); Layout.fillWidth: true
                           ToolTip.text: qsTr("Video data rate. Services publish their own limits; the warnings below say when you are over")
                           onCommitted: (v) => value = v }
                NumField { id: audioField; label: qsTr("audio"); suffix: qsTr("kb/s"); Layout.fillWidth: true; onCommitted: (v) => value = v; ToolTip.text: qsTr("Sound data rate; 160 is plenty for stereo") }
            }

            Fact { text: qsTr("Audio track") }
            ComboBox {
                ToolTip.text: qsTr("Which mixer track this stream sends")
                id: trackBox
                Layout.fillWidth: true
                model: [1, 2, 3, 4, 5, 6].map(n => qsTr("Track %1").arg(n))
            }

            Fact { text: qsTr("Limits"); visible: !sheet.custom }
            ColumnLayout {
                visible: !sheet.custom
                Layout.fillWidth: true
                CheckBox {
                    id: enforceCheck
                    objectName: "streamEnforce"
                    text: qsTr("Keep to %1's limits").arg(sheet.service.name ?? "")
                    ToolTip.text: qsTr("Lower the bitrate, keyframe interval and size to what the service publishes when these ask for more, and use an encoder it takes")
                }
                Note {
                    objectName: "streamAdjustments"
                    visible: enforceCheck.checked && text !== ""
                    text: backend.streamAdjustments(sheet.draft).join(" ")
                }
            }

            Item { implicitWidth: 1; visible: warnings.text !== "" }
            Label {
                id: warnings
                objectName: "streamWarnings"
                visible: text !== ""
                Layout.fillWidth: true
                text: backend.streamWarnings(sheet.draft).join("\n")
                color: Theme.warning
                font.pixelSize: Theme.fsLabel
                wrapMode: Text.Wrap
            }

            Fact { text: qsTr("Keyframes") }
            NumField { id: keyframeField; suffix: qsTr("s"); Layout.fillWidth: true; onCommitted: (v) => value = v; ToolTip.text: qsTr("Seconds between keyframes. Most services ask for 2") }

            Fact { text: qsTr("If it drops") }
            ColumnLayout {
                Layout.fillWidth: true
                CheckBox { id: reconnectCheck; text: qsTr("Reconnect"); ToolTip.text: qsTr("Try again when the connection drops") }
                RowLayout {
                    enabled: reconnectCheck.checked
                    Layout.fillWidth: true
                    NumField { id: retryField; suffix: qsTr("s"); Layout.fillWidth: true; onCommitted: (v) => value = v; ToolTip.text: qsTr("Seconds before the first retry; each wait after is a little longer") }
                    NumField { id: retriesField; label: qsTr("tries"); Layout.fillWidth: true; onCommitted: (v) => value = v; ToolTip.text: qsTr("How many times to try; 0 keeps trying until you end the stream") }
                }
                CheckBox { id: dynamicCheck; enabled: dynamicNote.text === ""; text: qsTr("Lower the bitrate when the network can't keep up"); ToolTip.text: qsTr("Reduce the encoder's bitrate when sending falls behind, and raise it again once the network recovers") }
                Note {
                    id: dynamicNote
                    objectName: "streamDynamicNote"
                    visible: text !== ""
                    text: backend.dynamicBitrateNote(sheet.draft)
                }
            }

            Fact { text: qsTr("Delay") }
            ColumnLayout {
                Layout.fillWidth: true
                NumField { id: delayField; objectName: "streamDelay"; suffix: qsTr("s"); Layout.fillWidth: true; onCommitted: (v) => value = v; ToolTip.text: qsTr("Hold the stream back by this many seconds before it goes out. It needs memory for that long at the bitrate") }
                CheckBox {
                    id: preserveCheck
                    objectName: "streamPreserveDelay"
                    enabled: delayField.value > 0 && reconnectCheck.checked
                    text: qsTr("Keep what it holds through a reconnect")
                    ToolTip.text: qsTr("After the connection drops, carry on from where viewers stopped seeing it instead of jumping to now. The delay grows by however long it was down")
                }
                Note {
                    objectName: "streamDelayMemory"
                    visible: delayField.value > 0
                    text: qsTr("Holds about %1 MB in memory.").arg(Math.max(1, Math.round((bitrateField.value + audioField.value) * 1000 / 8 * delayField.value / 1048576)))
                          + (preserveCheck.checked ? " " + qsTr("More while it is reconnecting, up to 1 GB.") : "")
                }
            }

            Fact { text: qsTr("Network") }
            ColumnLayout {
                Layout.fillWidth: true
                TextField {
                    ToolTip.text: qsTr("Send from this local address to choose a network (for example Ethernet over Wi-Fi); empty uses any")
                    id: bindField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Any address (a local IP to choose a network)")
                }
                ComboBox {
                    id: familyBox
                    objectName: "streamIpFamily"
                    Layout.fillWidth: true
                    enabled: bindField.text.trim() === ""
                    textRole: "label"
                    valueRole: "value"
                    model: [{ label: qsTr("IPv4 or IPv6"), value: 0 }, { label: qsTr("IPv4 only"), value: 4 }, { label: qsTr("IPv6 only"), value: 6 }]
                    Accessible.name: qsTr("IP version")
                    ToolTip.text: qsTr("Connect over one IP version only, for a network where the other is slow or broken. A send-from address decides it by itself")
                }
                TextField {
                    ToolTip.text: qsTr("Any of the container's or the connection's own options, such as pkt_size=1316 for UDP, set after ours. One nothing takes stops the stream and says which")
                    id: muxerOptionsField
                    objectName: "streamMuxerOptions"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Container options: key=value key=value")
                    font.family: Theme.monoFamily
                }
                CheckBox {
                    id: lowLatencyCheck
                    objectName: "streamLowLatency"
                    visible: sheet.custom ? /^rtmps?:/i.test(serverField.text.trim()) : (serverBox.currentValue ?? "").startsWith("rtmp")
                    text: qsTr("Low-latency sending")
                    ToolTip.text: qsTr("Send each packet the moment it is ready rather than gathering small ones (RTMP). A little less delay for a little more network overhead")
                }
            }
        }
    }
}
