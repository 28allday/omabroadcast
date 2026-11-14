import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Settings, one section at a time: the list on the left picks the page.
// Explanations live in each control's tooltip; a short note stays under a
// control only where it says something about the current state.
Sheet {
    id: sheet
    objectName: "settingsSheet"
    helpTopic: ({ video: "settings-video", audio: "settings-audio", meters: "mixer", recording: "settings-recording",
                  replay: "replay-buffer", streaming: "settings-streaming", camera: "virtual-camera",
                  hotkeys: "hotkeys", general: "settings-general" })[pages[page]?.id] ?? ""
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, Theme.wSheet + Theme.wSettingsNav + Theme.s6 * 2)
    height: Math.min(parent.height - Theme.s5 * 2, Theme.hSettings)
    modal: true
    title: qsTr("Settings")
    standardButtons: Dialog.Close

    readonly property var pages: [
        { id: "video", label: qsTr("Video") },
        { id: "audio", label: qsTr("Audio") },
        { id: "meters", label: qsTr("Meters") },
        { id: "recording", label: qsTr("Recording") },
        { id: "replay", label: qsTr("Replay buffer") },
        { id: "streaming", label: qsTr("Streaming") },
        { id: "camera", label: qsTr("Virtual camera") },
        { id: "hotkeys", label: qsTr("Hotkeys") },
        { id: "general", label: qsTr("General") }
    ]
    // The sound format holds while any output is open.
    readonly property bool outputsLive: backend.recording || backend.recordingStopping || backend.streaming
                                        || backend.replayActive || backend.virtualCamera
    // The page shown; kept while the app runs so Settings reopens where you were.
    property int page: 0
    function showPage(id) {
        const i = pages.findIndex(p => p.id === id)
        if (i >= 0) page = i
        open()
    }
    // Stream → Stream settings…
    function showStreaming() { showPage("streaming") }

    StreamSheet { id: streamSheet }

    component Fact: Label {
        color: Theme.textSecondary
        Layout.preferredWidth: Theme.wFactLabel
        Layout.alignment: Qt.AlignTop
        topPadding: Theme.s1
    }
    component Note: Label {
        Layout.fillWidth: true
        color: Theme.textMuted
        font.pixelSize: Theme.fsLabel
        wrapMode: Text.Wrap
    }
    // One page: a scrolling two-column grid of facts and controls.
    component Page: ScrollView {
        id: pageView
        default property alias content: grid.data
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        GridLayout {
            id: grid
            width: pageView.availableWidth - Theme.s3
            columns: 2
            columnSpacing: Theme.s4
            rowSpacing: Theme.s3
        }
    }

    contentItem: RowLayout {
        spacing: 0

        ListView {
            id: nav
            objectName: "settingsNav"
            Layout.preferredWidth: Theme.wSettingsNav
            Layout.fillHeight: true
            model: sheet.pages
            currentIndex: sheet.page
            keyNavigationEnabled: true
            onCurrentIndexChanged: sheet.page = currentIndex
            delegate: ItemDelegate {
                required property var modelData
                required property int index
                width: ListView.view.width
                implicitHeight: Theme.hRow
                text: modelData.label
                highlighted: index === sheet.page
                onClicked: sheet.page = index
                objectName: "settingsPage_" + modelData.id
                ToolTip.text: qsTr("%1 settings").arg(modelData.label)
                Accessible.name: qsTr("%1 settings").arg(modelData.label)
            }
        }
        Rectangle { Layout.fillHeight: true; implicitWidth: Theme.hairline; color: Theme.border }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Theme.s4
            currentIndex: sheet.page

            // ── Video ────────────────────────────────────────────────────
            Page {
                objectName: "settingsVideo"
                Fact { text: qsTr("Canvas") }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s1
                    ComboBox {
                        id: canvasBox
                        objectName: "canvasBox"
                        Layout.fillWidth: true
                        readonly property var sizes: [[1280, 720], [1920, 1080], [2560, 1440], [3840, 2160]]
                        readonly property int match: sizes.findIndex(s => s[0] === backend.canvasWidth && s[1] === backend.canvasHeight)
                        model: ["1280 × 720", "1920 × 1080", "2560 × 1440", "3840 × 2160", qsTr("Custom")]
                        // A size refused while an output runs would stay shown over the real one.
                        enabled: !sheet.outputsLive
                        currentIndex: match >= 0 ? match : 4
                        onActivated: if (currentIndex < 4) backend.setCanvasSize(sizes[currentIndex][0], sizes[currentIndex][1])
                        Accessible.name: qsTr("Canvas size")
                        ToolTip.text: qsTr("The size of the picture you compose, record and stream. Studios scale with it. Match it to your main source: a 1080p camera on a 4K canvas is only upscaled. Available while nothing is recording, streaming, buffering a replay or on the virtual camera.")
                    }
                    RowLayout {
                        visible: canvasBox.currentIndex === 4
                        Layout.fillWidth: true
                        NumField { label: qsTr("W"); value: backend.canvasWidth; Layout.fillWidth: true
                                   ToolTip.text: qsTr("Canvas width in pixels")
                                   onCommitted: (v) => backend.setCanvasSize(Math.round(v), backend.canvasHeight) }
                        NumField { label: qsTr("H"); value: backend.canvasHeight; Layout.fillWidth: true
                                   ToolTip.text: qsTr("Canvas height in pixels")
                                   onCommitted: (v) => backend.setCanvasSize(backend.canvasWidth, Math.round(v)) }
                    }
                    Note {
                        visible: backend.canvasFollowsDisplay
                        text: qsTr("Set from the first display you capture until you choose one.")
                    }
                }

                Fact { text: qsTr("Frame rate") }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s1
                    ComboBox {
                        id: frameRateBox
                        objectName: "frameRateBox"
                        Layout.fillWidth: true
                        // [frames, per seconds]: the common rates, then the NTSC ones.
                        readonly property var rates: [[24, 1], [25, 1], [30, 1], [48, 1], [50, 1], [60, 1], [120, 1],
                                                      [24000, 1001], [30000, 1001], [60000, 1001]]
                        readonly property int match: rates.findIndex(r => r[0] === backend.fpsNum && r[1] === backend.fpsDen)
                        model: ["24", "25", "30", "48", "50", "60", "120", "23.976", "29.97", "59.94", qsTr("Custom")]
                        currentIndex: match >= 0 ? match : rates.length
                        onActivated: if (currentIndex < rates.length) backend.setFrameRate(rates[currentIndex][0], rates[currentIndex][1])
                        Accessible.name: qsTr("Frame rate")
                        ToolTip.text: qsTr("Frames per second for the canvas, recordings and streams. 60 for games and fast motion; 30 for talking and slides; 25 or 50 to match European video; 29.97 and 59.94 for American broadcast. Choosing a capture device's mode at another rate sets this to it.")
                    }
                    Note {
                        objectName: "frameRateNote"
                        visible: text !== ""
                        text: backend.frameRateNote
                        color: Theme.warning
                    }
                    RowLayout {
                        visible: frameRateBox.currentIndex === frameRateBox.rates.length
                        Layout.fillWidth: true
                        NumField { objectName: "frameRateNum"; label: qsTr("Frames"); value: backend.fpsNum; Layout.fillWidth: true
                                   ToolTip.text: qsTr("Frames in the time below: 30000 frames in 1001 seconds is 29.97 a second.")
                                   onCommitted: (v) => backend.setFrameRate(Math.round(v), backend.fpsDen) }
                        NumField { objectName: "frameRateDen"; label: qsTr("Seconds"); value: backend.fpsDen; Layout.fillWidth: true
                                   ToolTip.text: qsTr("The seconds those frames take; 1 for a whole number of frames a second.")
                                   onCommitted: (v) => backend.setFrameRate(backend.fpsNum, Math.round(v)) }
                    }
                }

                Fact { text: qsTr("Scaling") }
                ComboBox {
                    id: downscaleBox
                    objectName: "downscaleBox"
                    Layout.fillWidth: true
                    model: [{ value: "area", label: qsTr("Area") }, { value: "bilinear", label: qsTr("Bilinear") },
                            { value: "bicubic", label: qsTr("Bicubic") }, { value: "lanczos", label: qsTr("Lanczos") }]
                    textRole: "label"
                    valueRole: "value"
                    currentIndex: Math.max(0, indexOfValue(backend.downscaleFilter))
                    onActivated: backend.downscaleFilter = currentValue
                    Accessible.name: qsTr("Scaling filter")
                    ToolTip.text: qsTr("How the canvas is scaled for a recording or stream at a different size. Area averages everything a pixel covers: smooth, no shimmer, the safe choice. Bicubic and Lanczos keep text and edges sharper (Lanczos most); Bilinear is the cheapest and softest, and can shimmer when shrinking a lot.")
                    Connections {
                        target: backend
                        function onSettingsChanged() { downscaleBox.currentIndex = Math.max(0, downscaleBox.indexOfValue(backend.downscaleFilter)) }
                    }
                }

                Fact { text: qsTr("Colour format") }
                ComboBox {
                    id: colourFormatBox
                    objectName: "colourFormatBox"
                    Layout.fillWidth: true
                    model: backend.colourFormats
                    textRole: "label"
                    valueRole: "value"
                    currentIndex: Math.max(0, indexOfValue(backend.colourFormat))
                    onActivated: backend.colourFormat = currentValue
                    Accessible.name: qsTr("Colour format")
                    ToolTip.text: qsTr("How the pictures are laid out for the encoder. Automatic uses the encoder's usual one, a 10-bit one for HDR. 4:2:0 halves the colour detail and plays everywhere; 4:4:4 keeps it for sharp text; 10 and 16 bits keep smooth gradients and HDR. Only what the recording encoder takes is offered.")
                    Connections {
                        target: backend
                        function onSettingsChanged() { colourFormatBox.currentIndex = Math.max(0, colourFormatBox.indexOfValue(backend.colourFormat)) }
                    }
                }

                Fact { text: qsTr("Colour space") }
                ComboBox {
                    id: colourSpaceBox
                    objectName: "colourSpaceBox"
                    Layout.fillWidth: true
                    model: [{ value: "srgb", label: qsTr("sRGB") }, { value: "601", label: qsTr("Rec. 601") },
                            { value: "709", label: qsTr("Rec. 709") }, { value: "pq", label: qsTr("Rec. 2100 (PQ)") },
                            { value: "hlg", label: qsTr("Rec. 2100 (HLG)") }]
                    textRole: "label"
                    valueRole: "value"
                    currentIndex: Math.max(0, indexOfValue(backend.colourSpace))
                    onActivated: backend.colourSpace = currentValue
                    Accessible.name: qsTr("Colour space")
                    ToolTip.text: qsTr("What the colours mean to the player. Rec. 709 is HD video and the safe choice; sRGB is the same picture labelled for computer screens; Rec. 601 is for SD video. Rec. 2100 is HDR, which needs a 10-bit format: PQ for HDR10 players and services, HLG for broadcast and screens that may be SDR.")
                    Connections {
                        target: backend
                        function onSettingsChanged() { colourSpaceBox.currentIndex = Math.max(0, colourSpaceBox.indexOfValue(backend.colourSpace)) }
                    }
                }

                Fact { text: qsTr("Range") }
                ComboBox {
                    id: colourRangeBox
                    objectName: "colourRangeBox"
                    Layout.fillWidth: true
                    model: [qsTr("Limited"), qsTr("Full")]
                    enabled: backend.colourFormat !== "bgra"
                    currentIndex: backend.colourFullRange || backend.colourFormat === "bgra" ? 1 : 0
                    onActivated: backend.colourFullRange = currentIndex === 1
                    Accessible.name: qsTr("Colour range")
                    ToolTip.text: backend.colourFormat === "bgra"
                                  ? qsTr("BGRA is always full range.")
                                  : qsTr("Limited keeps black at 16 and white at 235 (in 8 bits), as video players and TVs expect. Full uses every code; choose it only when whatever plays the file is set for it, or blacks and whites come out wrong.")
                }

                Fact { text: qsTr("SDR white") }
                NumField {
                    objectName: "sdrWhiteField"
                    Layout.fillWidth: true
                    value: backend.sdrWhite
                    suffix: qsTr("nits")
                    onCommitted: (v) => backend.sdrWhite = Math.round(v)
                    ToolTip.text: qsTr("How bright ordinary (SDR) white is in HDR: your desktop, cameras and pictures are placed at this level in an HDR recording, and HDR sources are measured against it. 203 is the broadcast reference; raise it if SDR looks dim beside HDR.")
                }

                Fact { text: qsTr("HDR peak") }
                NumField {
                    objectName: "hdrPeakField"
                    Layout.fillWidth: true
                    value: backend.hdrPeak
                    suffix: qsTr("nits")
                    onCommitted: (v) => backend.hdrPeak = Math.round(v)
                    ToolTip.text: qsTr("The brightest an HDR recording goes, and the display HLG is made for. PQ recordings are held at it and say so to players; 1000 suits most HDR screens.")
                }

                Item { implicitWidth: 1 }
                Note { objectName: "colourNote"; text: backend.colourNote }
            }

            // ── Audio ────────────────────────────────────────────────────
            Page {
                objectName: "settingsAudio"
                Fact { text: qsTr("Sample rate") }
                ComboBox {
                    objectName: "audioSampleRate"
                    Layout.fillWidth: true
                    model: ["44.1 kHz", "48 kHz"]
                    currentIndex: backend.audioSampleRate === 44100 ? 0 : 1
                    enabled: !sheet.outputsLive
                    onActivated: backend.audioSampleRate = currentIndex === 0 ? 44100 : 48000
                    ToolTip.text: qsTr("48 kHz is the video standard and what streaming services expect. Changing it restarts audio and media playback; available while nothing is recording, streaming, buffering a replay or on the virtual camera.")
                }
                Fact { text: qsTr("Channels") }
                ColumnLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        objectName: "audioChannelCount"
                        Layout.fillWidth: true
                        readonly property var counts: [1, 2, 3, 4, 5, 6, 8]
                        model: [qsTr("Mono"), qsTr("Stereo"), "2.1", "4.0", "4.1", "5.1", "7.1"]
                        currentIndex: counts.indexOf(backend.audioChannelCount)
                        enabled: !sheet.outputsLive
                        onActivated: backend.audioChannelCount = counts[currentIndex]
                        ToolTip.text: qsTr("Speaker layout for capture, the mixer, monitoring and recordings. Changing it restarts audio and media playback; available while nothing is recording, streaming, buffering a replay or on the virtual camera.")
                    }
                    Note {
                        visible: backend.audioChannelCount > 2
                        text: qsTr("Surround needs compatible players; stereo devices downmix monitoring.")
                    }
                }

                Fact { text: qsTr("Latency") }
                CheckBox {
                    objectName: "lowLatencyCheck"
                    text: qsTr("Low latency")
                    checked: backend.lowLatencyAudio
                    enabled: !sheet.outputsLive
                    onClicked: backend.lowLatencyAudio = checked
                    ToolTip.text: qsTr("Smaller audio buffers for responsive monitoring. Turn off if audio crackles. Sync offsets still add the buffering they ask for. Available while nothing is recording, streaming, buffering a replay or on the virtual camera.")
                }
            }

            // ── Meters ───────────────────────────────────────────────────
            Page {
                objectName: "settingsMeters"
                MeterControls { Layout.columnSpan: 2; Layout.fillWidth: true; labelWidth: Theme.wFactLabel; columnSpacing: Theme.s4 }
            }

            // ── Recording ────────────────────────────────────────────────
            Page {
                objectName: "settingsRecording"
                Fact { text: qsTr("Encoder") }
                ColumnLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        id: encoderBox
                        objectName: "recordingEncoder"
                        Layout.fillWidth: true
                        model: backend.encoders
                        textRole: "label"
                        valueRole: "name"
                        currentIndex: Math.max(0, indexOfValue(backend.encoder))
                        onActivated: backend.encoder = currentValue
                        ToolTip.text: qsTr("What turns the picture into a file. \"Best available\" picks the encoder that works best here for recording; the list shows only encoders that actually opened on this computer.")
                        Connections {
                            target: backend
                            function onSettingsChanged() { encoderBox.currentIndex = Math.max(0, encoderBox.indexOfValue(backend.encoder)) }
                        }
                    }
                    Note { text: backend.encoderNote }
                }

                Fact { text: qsTr("Size") }
                ComboBox {
                    id: recordingSizeBox
                    objectName: "recordingSize"
                    Layout.fillWidth: true
                    model: backend.outputSizes
                    textRole: "label"
                    valueRole: "height"
                    enabled: !backend.recording
                    currentIndex: Math.max(0, indexOfValue(backend.recordingHeight))
                    onActivated: backend.recordingHeight = currentValue
                    ToolTip.text: qsTr("The picture size recordings are made at, in the canvas's shape. Smaller than the canvas makes smaller files and is easier on the encoder; the picture is scaled on the graphics card.")
                    Connections {
                        target: backend
                        function onSettingsChanged() { recordingSizeBox.currentIndex = Math.max(0, recordingSizeBox.indexOfValue(backend.recordingHeight)) }
                    }
                }

                Fact { text: qsTr("Rate control"); visible: rateBox.visible }
                ComboBox {
                    id: rateBox
                    objectName: "rateControlBox"
                    visible: !backend.encoderIsForEditing
                    Layout.fillWidth: true
                    textRole: "label"
                    valueRole: "id"
                    model: [{ id: "quality", label: qsTr("Constant quality") }, { id: "cbr", label: qsTr("Constant bitrate") },
                            { id: "vbr", label: qsTr("Variable bitrate") }, { id: "cqp", label: qsTr("Fixed quantiser") },
                            { id: "lossless", label: qsTr("Lossless") }]
                    currentIndex: Math.max(0, indexOfValue(backend.recordingOptions.rateControl))
                    onActivated: backend.setRecordingOption("rateControl", currentValue)
                    ToolTip.text: qsTr("Constant quality keeps the look steady and lets the file size follow the picture (best for recordings). Constant and variable bitrate aim for a data rate. Fixed quantiser compresses every frame the same. Lossless keeps every pixel, at a very large size.")
                }

                // An editing codec has a quality tier instead of rate control.
                Fact { text: qsTr("Quality"); visible: tierBox.visible }
                ComboBox {
                    id: tierBox
                    objectName: "editingTierBox"
                    visible: backend.encoderIsForEditing
                    Layout.fillWidth: true
                    textRole: "label"
                    valueRole: "id"
                    model: [{ id: "light", label: qsTr("Light · about ProRes LT") },
                            { id: "standard", label: qsTr("Standard · about ProRes 422") },
                            { id: "high", label: qsTr("High · about ProRes HQ") }]
                    currentIndex: Math.max(0, indexOfValue(backend.recordingOptions.profile.toLowerCase()))
                    onActivated: backend.setRecordingOption("profile", currentValue)
                    ToolTip.text: qsTr("APV keeps the quality steady and lets the file size follow the picture. Each step is at least as good as the ProRes level it names, at about its size on average: camera pictures come out larger, screen recordings smaller.")
                }

                Fact { text: qsTr("Quality"); visible: qualityRow.visible }
                RowLayout {
                    id: qualityRow
                    visible: rateBox.visible && ["quality", "cqp"].includes(backend.recordingOptions.rateControl)
                    Layout.fillWidth: true
                    spacing: Theme.s2
                    Slider {
                        Layout.fillWidth: true
                        from: 10; to: 35; stepSize: 1
                        value: backend.quality
                        onMoved: backend.quality = value
                        ToolTip.text: qsTr("Lower is better and bigger: 18 is near lossless, 23 a good default, 30 small files.")
                    }
                    // Room for its longest words, so the slider stays put as it moves.
                    Label {
                        text: backend.quality + (backend.quality <= 18 ? qsTr("  near lossless") : backend.quality >= 30 ? qsTr("  small files") : "")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fsLabel
                        Layout.preferredWidth: qualityMetrics.advanceWidth(qsTr("35  near lossless")) + Theme.s1
                        FontMetrics { id: qualityMetrics; font.pixelSize: Theme.fsLabel }
                    }
                }

                Fact { text: qsTr("Bitrate"); visible: bitrateRow.visible }
                RowLayout {
                    id: bitrateRow
                    visible: rateBox.visible && ["cbr", "vbr"].includes(backend.recordingOptions.rateControl)
                    Layout.fillWidth: true
                    NumField { objectName: "recordingBitrate"; suffix: qsTr("kb/s"); value: backend.recordingOptions.bitrateKbps; Layout.fillWidth: true
                               ToolTip.text: qsTr("Video data rate in kilobits per second. 1080p60 looks good from about 12 000 for recordings.")
                               onCommitted: (v) => backend.setRecordingOption("bitrateKbps", Math.round(v)) }
                    NumField { visible: backend.recordingOptions.rateControl === "vbr"; label: qsTr("peak"); suffix: qsTr("kb/s"); value: backend.recordingOptions.maxKbps
                               Layout.fillWidth: true; ToolTip.text: qsTr("The most variable bitrate may reach in busy, fast-moving pictures; 0 allows half as much again as the target.")
                               onCommitted: (v) => backend.setRecordingOption("maxKbps", Math.round(v)) }
                }

                Fact { text: qsTr("Format") }
                ColumnLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        id: formatBox
                        objectName: "recordingFormat"
                        Layout.fillWidth: true
                        model: backend.fileFormats
                        textRole: "label"
                        valueRole: "id"
                        enabled: !backend.recording
                        currentIndex: Math.max(0, indexOfValue(backend.container))
                        onActivated: backend.container = currentValue
                        ToolTip.text: qsTr("The kind of file. MKV is safest. Hybrid MP4 is crash-safe while recording and an ordinary MP4 afterwards. Plain MP4 and MOV need a clean stop. HLS writes a playlist with segments beside it. Custom takes any container FFmpeg can write.")
                    }
                    TextField {
                        id: customMuxerField
                        objectName: "customMuxer"
                        visible: backend.container === "custom"
                        enabled: !backend.recording
                        Layout.fillWidth: true
                        placeholderText: qsTr("FFmpeg container, such as nut, webm or avi")
                        font.family: Theme.monoFamily
                        text: backend.customMuxer
                        onEditingFinished: backend.customMuxer = text
                        ToolTip.text: qsTr("Any container FFmpeg can write, by its own name (ffmpeg -muxers lists them). The file takes its usual extension.")
                    }
                    Note {
                        objectName: "recordingFormatNote"
                        readonly property var format: backend.fileFormats.find(f => f.id === backend.container) ?? {}
                        readonly property var custom: backend.container === "custom" ? backend.muxerInfo(backend.customMuxer) : ({})
                        text: backend.container !== "custom"
                            ? (format.survivesCrash ? qsTr("Plays even if cut off by a crash.") : qsTr("Needs a clean stop to play."))
                              + " " + (format.chapters ? qsTr("Keeps chapters.") : qsTr("No chapters."))
                            : backend.customMuxer === "" ? qsTr("Name the container to record into.")
                            : custom.problem !== "" ? custom.problem
                            : qsTr("%1, saved as .%2.").arg(custom.label).arg(custom.extension)
                              + " " + (custom.chapters ? qsTr("Keeps chapters.") : qsTr("No chapters."))
                              + " " + qsTr("Whether it plays after a crash depends on the container.")
                    }
                    CheckBox {
                        objectName: "remuxToMp4"
                        readonly property bool alreadyMp4: ["mp4", "mov", "fmp4", "fmov", "hmp4", "hmov", "hls"].includes(backend.container === "custom" ? backend.customMuxer : backend.container)
                        visible: !alreadyMp4
                        text: qsTr("Also make an MP4 copy when it ends")
                        checked: backend.remuxToMp4
                        onClicked: backend.remuxToMp4 = checked
                        ToolTip.text: qsTr("Record safely in this format, then get an MP4 beside it for editors and players that want one. Nothing is encoded again; File → Remux recordings… does the same for older files.")
                    }
                }

                Fact { text: qsTr("Audio codec") }
                ComboBox {
                    objectName: "recordingAudioCodec"
                    Layout.fillWidth: true
                    textRole: "label"
                    valueRole: "id"
                    model: [{ id: "aac", label: qsTr("AAC · plays everywhere") }, { id: "opus", label: qsTr("Opus · best at low bitrates") },
                            { id: "flac", label: qsTr("FLAC · lossless") }, { id: "alac", label: qsTr("ALAC · lossless, Apple") },
                            { id: "pcm16", label: qsTr("Uncompressed 16-bit (PCM)") },
                            { id: "pcm24", label: qsTr("Uncompressed 24-bit (PCM)") }, { id: "pcm32f", label: qsTr("Uncompressed 32-bit float (PCM)") }]
                    currentIndex: Math.max(0, indexOfValue(backend.recordingOptions.audioCodec))
                    onActivated: backend.setRecordingOption("audioCodec", currentValue)
                    ToolTip.text: qsTr("AAC plays everywhere. FLAC, ALAC and PCM keep the sound exactly, for editing afterwards. Not every format takes every codec; the recording says so if it can't.")
                }

                Fact { text: qsTr("Audio tracks") }
                ColumnLayout {
                    Layout.fillWidth: true
                    AudioTracks {
                        objectName: "recordingTracks"
                        Layout.fillWidth: true
                        mask: backend.recordingTracks
                        enabled: !backend.recording
                        onChosen: (mask) => backend.recordingTracks = mask
                    }
                    Note {
                        visible: backend.recordingTracks === 0
                        text: qsTr("Video only — no audio tracks selected.")
                    }
                }

                Fact { text: qsTr("New file") }
                RowLayout {
                    Layout.fillWidth: true
                    NumField { objectName: "splitMinutes"; suffix: qsTr("min"); value: backend.recordingOptions.splitMinutes; Layout.fillWidth: true
                               ToolTip.text: qsTr("Start a new file every so many minutes; 0 never does. Each starts on a keyframe and nothing is lost or repeated between files. Record → Start a new file splits by hand.")
                               onCommitted: (v) => backend.setRecordingOption("splitMinutes", Math.round(v)) }
                    // HLS is written as many small files: no one of them to measure.
                    NumField { visible: backend.container !== "hls"; label: qsTr("or"); suffix: qsTr("MB"); value: backend.recordingOptions.splitMegabytes; Layout.fillWidth: true
                               ToolTip.text: qsTr("Or a new file once it reaches this many megabytes; 0 never does.")
                               onCommitted: (v) => backend.setRecordingOption("splitMegabytes", Math.round(v)) }
                }

                Fact { text: qsTr("Folder") }
                RowLayout {
                    Layout.fillWidth: true
                    TextField {
                        objectName: "recordingFolder"
                        Layout.fillWidth: true
                        text: backend.folder
                        onEditingFinished: backend.folder = text
                        ToolTip.text: qsTr("Where recordings go. Empty uses Videos/OmaBroadcast.")
                    }
                    Button {
                        objectName: "chooseRecordingFolder"
                        text: qsTr("Choose…")
                        icon.name: "folder-open"
                        ToolTip.text: qsTr("Pick the folder recordings go to")
                        onClicked: backend.chooseRecordingFolder()
                    }
                }

                Fact { text: qsTr("File name") }
                ColumnLayout {
                    Layout.fillWidth: true
                    TextField {
                        id: fileNameField
                        objectName: "fileNamePattern"
                        Layout.fillWidth: true
                        text: backend.fileNamePattern
                        onEditingFinished: backend.fileNamePattern = text
                        Accessible.name: qsTr("File name pattern")
                        ToolTip.text: qsTr("Date codes: %Y year, %y short year, %m month, %d day, %H hour, %M minute, %S second. Names: {studio}, {profile}, {collection}, {width}, {height}, {fps}. An existing file is never overwritten; the new one gets a number.")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            objectName: "fileNamePreview"
                            Layout.fillWidth: true
                            // Ticks once a second so the date codes stay current.
                            property int tick: 0
                            Timer { interval: 1000; running: sheet.visible; repeat: true; onTriggered: parent.tick++ }
                            text: (tick, backend.fileNamePreview(fileNameField.text, noSpacesCheck.checked))
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.fsLabel
                            color: Theme.textSecondary
                            elide: Text.ElideMiddle
                        }
                        CheckBox {
                            id: noSpacesCheck
                            objectName: "fileNameNoSpaces"
                            text: qsTr("No spaces")
                            checked: backend.fileNameNoSpaces
                            onClicked: backend.fileNameNoSpaces = checked
                            ToolTip.text: qsTr("Use _ instead of spaces, for tools and servers that dislike them.")
                        }
                    }
                }

                Fact { text: qsTr("Advanced") }
                Button {
                    id: advancedToggle
                    objectName: "recordingAdvanced"
                    property bool open: false
                    text: open ? qsTr("Hide") : qsTr("Show")
                    icon.name: open ? "chevron-up" : "chevron-down"
                    onClicked: open = !open
                    ToolTip.text: qsTr("Keyframes, B-frames, look-ahead, the encoder's own preset, profile, tune and options, and the container's options.")
                }
                Fact { visible: advancedToggle.open; text: qsTr("Keyframes") }
                RowLayout {
                    visible: advancedToggle.open
                    Layout.fillWidth: true
                    NumField { suffix: qsTr("s"); value: backend.recordingOptions.keyframeSeconds; Layout.fillWidth: true
                               ToolTip.text: qsTr("Seconds between keyframes, where playback can start and files can split. 0 leaves it to the encoder.")
                               onCommitted: (v) => backend.setRecordingOption("keyframeSeconds", Math.round(v)) }
                    NumField { label: qsTr("B-frames"); value: backend.recordingOptions.bFrames; Layout.fillWidth: true
                               ToolTip.text: qsTr("Frames that look both ways: smaller files, a little more delay. 0 to 4.")
                               onCommitted: (v) => backend.setRecordingOption("bFrames", Math.round(v)) }
                    NumField { label: qsTr("look-ahead"); value: backend.recordingOptions.lookahead; Layout.fillWidth: true
                               ToolTip.text: qsTr("Frames the encoder studies before deciding, where it can: better quality for more delay. 0 is off.")
                               onCommitted: (v) => backend.setRecordingOption("lookahead", Math.round(v)) }
                }
                Fact { visible: advancedToggle.open; text: qsTr("Preset") }
                RowLayout {
                    visible: advancedToggle.open
                    Layout.fillWidth: true
                    TextField { placeholderText: qsTr("preset"); text: backend.recordingOptions.preset; Layout.fillWidth: true
                                ToolTip.text: qsTr("The encoder's own preset name, such as p5 (NVIDIA) or veryfast (x264). Empty uses ours.")
                                onEditingFinished: backend.setRecordingOption("preset", text) }
                    TextField { placeholderText: qsTr("profile"); text: backend.recordingOptions.profile; Layout.fillWidth: true
                                ToolTip.text: qsTr("The encoder's profile, such as high or main. Empty uses its default; for APV it is the quality above (light, standard or high).")
                                onEditingFinished: backend.setRecordingOption("profile", text) }
                    TextField { placeholderText: qsTr("tune"); text: backend.recordingOptions.tune; Layout.fillWidth: true
                                ToolTip.text: qsTr("The encoder's tune, such as hq or film. Empty uses ours.")
                                onEditingFinished: backend.setRecordingOption("tune", text) }
                }
                Fact { visible: splitBox.visible; text: qsTr("Split encode") }
                ComboBox {
                    id: splitBox
                    objectName: "splitEncode"
                    visible: advancedToggle.open && backend.splitEncodeModes.length > 0
                    Layout.fillWidth: true
                    model: backend.splitEncodeModes
                    textRole: "label"
                    valueRole: "value"
                    currentIndex: Math.max(0, indexOfValue(backend.recordingOptions.splitEncode))
                    onActivated: backend.setRecordingOption("splitEncode", currentValue)
                    ToolTip.text: qsTr("Share each picture between the encoder's engines, in strips, for sizes and rates one engine can't keep up with. Only some cards have more than one.")
                }
                Fact { visible: advancedToggle.open; text: qsTr("Options") }
                TextField {
                    objectName: "encoderOptions"
                    visible: advancedToggle.open
                    Layout.fillWidth: true
                    placeholderText: "key=value key=value"
                    font.family: Theme.monoFamily
                    text: backend.recordingOptions.options
                    onEditingFinished: backend.setRecordingOption("options", text)
                    ToolTip.text: qsTr("Any of the encoder's own options, passed as typed. One it does not know stops the recording from starting and says which.")
                }
                Fact { visible: advancedToggle.open; text: qsTr("Container options") }
                TextField {
                    objectName: "containerOptions"
                    visible: advancedToggle.open
                    enabled: !backend.recording
                    Layout.fillWidth: true
                    placeholderText: "key=value key=value"
                    font.family: Theme.monoFamily
                    text: backend.containerOptions
                    onEditingFinished: backend.containerOptions = text
                    ToolTip.text: qsTr("Any of the container's own options, such as reserve_index_space=1M for MKV, set after ours. One it does not know stops the recording from starting and says which.")
                }
            }

            // ── Replay buffer ────────────────────────────────────────────
            Page {
                objectName: "settingsReplay"
                Fact { text: qsTr("Replay buffer") }
                ColumnLayout {
                    Layout.fillWidth: true
                    Button {
                        objectName: "replayToggle"
                        text: backend.replayActive ? qsTr("Stop the replay buffer") : qsTr("Start the replay buffer")
                        icon.name: backend.replayActive ? "square" : "rotate-ccw"
                        onClicked: backend.toggleReplay()
                        ToolTip.text: qsTr("Keeps the last stretch of the program in memory, encoded as the Recording page says, so you can save something that has just happened. Record → Save replay or Ctrl+Shift+R saves it.")
                    }
                    Note {
                        text: backend.replayActive ? qsTr("Holding %1.").arg(backend.replayHeld)
                                                   : qsTr("Nothing is kept until it is started.")
                    }
                }

                Fact { text: qsTr("Keep") }
                ColumnLayout {
                    Layout.fillWidth: true
                    RowLayout {
                        Layout.fillWidth: true
                        NumField { objectName: "replaySeconds"; suffix: qsTr("s"); value: backend.replayOptions.seconds; Layout.fillWidth: true
                                   enabled: !backend.replayActive
                                   ToolTip.text: qsTr("How far back a save reaches. It starts on a keyframe, so a save can hold up to one keyframe interval more. 0 keeps as much as the size allows.")
                                   onCommitted: (v) => backend.setReplayOption("seconds", Math.round(v)) }
                        NumField { objectName: "replayMegabytes"; label: qsTr("at most"); suffix: qsTr("MB"); value: backend.replayOptions.megabytes; Layout.fillWidth: true
                                   enabled: !backend.replayActive
                                   ToolTip.text: qsTr("The most memory it may use; the oldest part goes first when it is full. 0 sets no limit.")
                                   onCommitted: (v) => backend.setReplayOption("megabytes", Math.round(v)) }
                    }
                    Note {
                        text: backend.replayEstimate !== ""
                              ? qsTr("Memory: %1.").arg(backend.replayEstimate)
                              : qsTr("Memory depends on the picture with this rate control; the status line shows what it holds while it runs.")
                    }
                    Note { visible: backend.replayActive; text: qsTr("Stop the replay buffer to change these.") }
                }

                Fact { text: qsTr("File name") }
                ColumnLayout {
                    Layout.fillWidth: true
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: replayPrefixField
                            objectName: "replayPrefix"
                            Layout.fillWidth: true
                            placeholderText: qsTr("Before")
                            text: backend.replayOptions.prefix
                            onEditingFinished: backend.setReplayOption("prefix", text)
                            Accessible.name: qsTr("Replay file name prefix")
                            ToolTip.text: qsTr("Put before the Recording page's file name pattern. It may hold the same date codes and names.")
                        }
                        TextField {
                            id: replaySuffixField
                            objectName: "replaySuffix"
                            Layout.fillWidth: true
                            placeholderText: qsTr("After")
                            text: backend.replayOptions.suffix
                            onEditingFinished: backend.setReplayOption("suffix", text)
                            Accessible.name: qsTr("Replay file name suffix")
                            ToolTip.text: qsTr("Put after the file name pattern.")
                        }
                    }
                    Label {
                        objectName: "replayNamePreview"
                        Layout.fillWidth: true
                        property int tick: 0
                        Timer { interval: 1000; running: sheet.visible; repeat: true; onTriggered: parent.tick++ }
                        text: (tick, backend.fileNamePreview(replayPrefixField.text + backend.fileNamePattern + replaySuffixField.text,
                                                             backend.fileNameNoSpaces))
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textSecondary
                        elide: Text.ElideMiddle
                    }
                    Note { text: qsTr("Saved in the recordings folder, in the recording format.") }
                }
            }

            // ── Streaming ────────────────────────────────────────────────
            Page {
                objectName: "settingsStreaming"
                Fact { text: qsTr("Streams") }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s1
                    Repeater {
                        model: backend.streamTargets
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            CheckBox {
                                objectName: "streamEnabled"
                                checked: modelData.enabled
                                enabled: !backend.streaming
                                onClicked: backend.saveStreamTarget({ id: modelData.id, enabled: checked }, "", false)
                                Accessible.name: qsTr("Go live to %1").arg(label.text)
                                ToolTip.text: qsTr("Include this stream when you go live")
                            }
                            Label {
                                id: label
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                readonly property var service: backend.streamServices.find(s => s.id === modelData.service) ?? {}
                                text: modelData.name || service.name || qsTr("Custom server")
                                color: modelData.enabled ? Theme.textPrimary : Theme.textMuted
                            }
                            Button {
                                text: qsTr("Edit…")
                                enabled: !backend.streaming
                                onClicked: streamSheet.edit(modelData)
                                ToolTip.text: qsTr("Change where this stream goes and how it is sent")
                            }
                            ToolButton {
                                objectName: "removeStreamButton"
                                icon.name: "trash-2"
                                enabled: !backend.streaming
                                // Its key goes from the keyring too: asked first.
                                onClicked: { removeStreamSheet.target = modelData.id; removeStreamSheet.label = label.text; removeStreamSheet.open() }
                                ToolTip.text: qsTr("Remove this stream and its key")
                                Accessible.name: ToolTip.text
                            }
                        }
                    }
                    Note {
                        visible: backend.streamTargets.length === 0
                        text: qsTr("No streams yet. Add one for each place you go live to.")
                    }
                    Button {
                        objectName: "addStreamButton"
                        text: qsTr("Add a stream…")
                        icon.name: "plus"
                        enabled: !backend.streaming
                        onClicked: streamSheet.edit({})
                        ToolTip.text: qsTr("A service such as Twitch or YouTube, or a server of your own. Every stream switched on goes out together.")
                    }
                    Label {
                        visible: !backend.keyringAvailable
                        Layout.fillWidth: true
                        text: qsTr("No keyring is running, so stream keys are kept in the profile file. Start one (gnome-keyring or KWallet) to keep them out of it.")
                        color: Theme.warning
                        font.pixelSize: Theme.fsLabel
                        wrapMode: Text.Wrap
                    }
                }

                Fact { text: qsTr("Recording") }
                ColumnLayout {
                    Layout.fillWidth: true
                    CheckBox {
                        objectName: "recordWhileStreaming"
                        text: qsTr("Record while streaming")
                        checked: backend.recordWhileStreaming
                        onClicked: backend.recordWhileStreaming = checked
                        ToolTip.text: qsTr("Start a recording when you go live, with the Recording page's settings.")
                    }
                    CheckBox {
                        text: qsTr("Stop recording when the stream ends")
                        checked: backend.stopRecordingWithStream
                        onClicked: backend.stopRecordingWithStream = checked
                        ToolTip.text: qsTr("Otherwise the recording carries on until you stop it.")
                    }
                    CheckBox {
                        objectName: "replayWhileStreaming"
                        text: qsTr("Keep a replay buffer while streaming")
                        checked: backend.replayWhileStreaming
                        onClicked: backend.replayWhileStreaming = checked
                        ToolTip.text: qsTr("Start the replay buffer when you go live and stop it when the stream ends, so a moment from the stream can be saved.")
                    }
                }
            }

            // ── Virtual camera ───────────────────────────────────────────
            Page {
                objectName: "settingsCamera"
                Fact { text: qsTr("Virtual camera") }
                ColumnLayout {
                    Layout.fillWidth: true
                    Button {
                        objectName: "cameraToggle"
                        text: backend.virtualCamera ? qsTr("Stop the virtual camera") : qsTr("Start the virtual camera")
                        icon.name: "camera"
                        onClicked: backend.toggleVirtualCamera()
                        ToolTip.text: qsTr("Offers a camera called \"OmaBroadcast Camera\" to browsers and calling apps.")
                    }
                    Note {
                        text: !backend.virtualCamera ? qsTr("Apps can only see it while it is on.")
                              : backend.virtualCameraInUse ? qsTr("An app is using it.")
                              : qsTr("On; no app is using it yet.")
                    }
                }

                Fact { text: qsTr("Shows") }
                ColumnLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        id: cameraShowsBox
                        objectName: "cameraShows"
                        Layout.fillWidth: true
                        model: backend.cameraChoices
                        textRole: "label"
                        function sync() {
                            const s = backend.cameraShows
                            currentIndex = Math.max(0, model.findIndex(c => c.shows === s.shows && c.target === s.target))
                        }
                        onModelChanged: sync()
                        Component.onCompleted: sync()
                        onActivated: (i) => backend.setCameraShows(model[i].shows, model[i].target)
                        Connections { target: backend; function onVirtualCameraChanged() { cameraShowsBox.sync() } }
                        Accessible.name: qsTr("What the virtual camera shows")
                        ToolTip.text: qsTr("The program as it goes out, the studio mode preview, another studio while you switch the live one, or one source on its own, fitted to the canvas.")
                    }
                    Note {
                        visible: backend.cameraShows.gone
                        text: qsTr("What it showed is no longer in this collection, so it shows the program.")
                    }
                    Note { text: qsTr("At the canvas size and frame rate.") }
                }
            }

            // ── Hotkeys ──────────────────────────────────────────────────
            HotkeysPage { objectName: "settingsHotkeys" }

            // ── General ──────────────────────────────────────────────────
            Page {
                objectName: "settingsGeneral"
                Fact { text: qsTr("Graphics") }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s1
                    ComboBox {
                        id: gpuBox
                        objectName: "gpuBox"
                        Layout.fillWidth: true
                        readonly property bool busy: backend.recording || backend.recordingStopping || backend.streaming
                                                     || backend.replayActive || backend.virtualCamera
                        model: backend.gpuChoices
                        textRole: "label"
                        valueRole: "value"
                        enabled: !busy
                        // Only once it is made: until then indexOfValue finds nothing.
                        Component.onCompleted: currentIndex = Math.max(0, indexOfValue(backend.gpu))
                        onActivated: backend.gpu = currentValue
                        Accessible.name: qsTr("GPU that draws the picture")
                        ToolTip.text: busy ? qsTr("Stop recording, streaming, the replay buffer and the virtual camera to change it: the picture restarts on the new GPU.")
                                           : qsTr("The GPU that puts the studio together. Automatic uses a discrete GPU, or the integrated one when the machine is on battery (decided when the picture starts), and encoding stays on the same GPU when it can. Processor only is slow, for comparing.")
                        Connections {
                            target: backend
                            function onPreferencesChanged() { gpuBox.currentIndex = Math.max(0, gpuBox.indexOfValue(backend.gpu)) }
                        }
                    }
                    Note {
                        objectName: "gpuInUse"
                        text: backend.engineWhy ? qsTr("In use: %1 — %2").arg(backend.engineName).arg(backend.engineWhy)
                                                : qsTr("In use: %1").arg(backend.engineName || "—")
                    }
                }

                Fact { text: qsTr("Screen capture") }
                ColumnLayout {
                    Layout.fillWidth: true
                    CheckBox {
                        objectName: "hideFromCaptureCheck"
                        text: qsTr("Hide OmaBroadcast from screen capture")
                        checked: backend.hideFromCapture
                        enabled: backend.captureHidingAvailable
                        onClicked: backend.hideFromCapture = checked
                        ToolTip.text: qsTr("Its windows show as black in display captures, here and in other apps, so the preview never repeats inside itself. Screenshots show them black too.")
                    }
                    Note { visible: !backend.captureHidingAvailable; text: qsTr("Needs Hyprland.") }
                }

                Fact { text: backend.omarchyBarAvailable ? qsTr("Bar and tray") : qsTr("Tray") }
                ColumnLayout {
                    Layout.fillWidth: true
                    CheckBox {
                        objectName: "omarchyBarCheck"
                        visible: backend.omarchyBarAvailable
                        text: qsTr("Show an icon in the Omarchy bar")
                        checked: backend.omarchyBarActive
                        onClicked: backend.setOmarchyBar(checked)
                        ToolTip.text: qsTr("Click it to show or hide the window; right-click it to start and stop recording and streaming, and to quit. It takes the tray icon's place.")
                    }
                    CheckBox {
                        objectName: "trayIconCheck"
                        text: qsTr("Show an icon in the tray")
                        checked: backend.trayIcon
                        enabled: !backend.omarchyBarActive
                        onClicked: backend.trayIcon = checked
                        ToolTip.text: qsTr("Click it to show or hide the window; its menu starts and stops recording and streaming, and quits.")
                    }
                    CheckBox {
                        objectName: "closeToTrayCheck"
                        text: qsTr("Keep running when the window is closed")
                        checked: backend.closeToTray
                        enabled: backend.trayIcon || backend.omarchyBarActive
                        onClicked: backend.closeToTray = checked
                        ToolTip.text: qsTr("Closing the window hides it; quit from the icon's menu or File → Quit.")
                    }
                    CheckBox {
                        objectName: "startInTrayCheck"
                        text: qsTr("Start with the window hidden")
                        checked: backend.startInTray
                        enabled: backend.trayIcon || backend.omarchyBarActive
                        onClicked: backend.startInTray = checked
                        ToolTip.text: qsTr("After the launch window, the main window stays hidden; show it from the bar or tray icon.")
                    }
                }

                Fact { text: qsTr("Confirm") }
                ColumnLayout {
                    Layout.fillWidth: true
                    CheckBox {
                        objectName: "confirmStartCheck"
                        text: qsTr("Ask before starting a recording or a stream")
                        checked: backend.confirmStart
                        onClicked: backend.confirmStart = checked
                        ToolTip.text: qsTr("Covers the buttons, the menus, Ctrl+R and the bar or tray icon.")
                    }
                    CheckBox {
                        objectName: "confirmStopCheck"
                        text: qsTr("Ask before stopping a recording or a stream")
                        checked: backend.confirmStop
                        onClicked: backend.confirmStop = checked
                        ToolTip.text: qsTr("Covers the buttons, the menus, Ctrl+R and the bar or tray icon. Quitting while recording or live always asks.")
                    }
                }
            }
        }
    }

    Sheet {
        id: removeStreamSheet
        objectName: "removeStreamSheet"
        property string target: ""
        property string label: ""
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s5 * 2, 420)
        modal: true
        title: qsTr("Remove this stream?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        contentItem: Label {
            text: qsTr("Remove “%1”? Its stream key is deleted from the keyring as well.").arg(removeStreamSheet.label)
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }
        onAccepted: backend.removeStreamTarget(target)
    }
}
