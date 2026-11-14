import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtQuick.Layouts
import OmaBroadcast 1.0

// A source's video filters: add, remove, reorder, switch off, edit, copy and
// paste. Every change is one undo step through the source's settings.
Sheet {
    id: sheet
    helpTopic: "filters"
    objectName: "videoFiltersSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: qsTr("Filters for %1").arg(source.name ?? "")
    width: Math.min(1120, parent ? parent.width - Theme.s4 : 1120)
    height: Math.min(820, parent ? parent.height - Theme.s4 : 820)
    standardButtons: Dialog.Close
    property string sourceId: ""
    property var generation: 0
    property var source: ({})
    property var filters: []
    property var problems: []
    property int selectedIndex: 0
    readonly property var selectedFilter: filters[selectedIndex] ?? null
    readonly property var definition: selectedFilter ? backend.videoFilterTypes.find(d => d.type === selectedFilter.type) ?? null : null
    // Video delay only where the pictures come on their own clock.
    // Colour correction is not offered any more: Colour wheels does its work. One
    // already in a chain still runs and can still be edited.
    readonly property var addable: backend.videoFilterTypes.filter(d => d.type !== "colour" && (!d.async || source.async === true))

    function showSource(id) {
        sourceId = id
        generation = backend.collectionGeneration()
        selectedIndex = 0
        matteCheck.checked = false
        refresh()
        if (source.kind !== undefined) open()
    }
    function refresh() {
        source = generation === backend.collectionGeneration() ? backend.videoFilterSource(sourceId) : ({})
        if (source.kind === undefined) {
            close()
            return
        }
        filters = backend.videoFilters(sourceId)
        problems = backend.videoFilterProblems(sourceId)
        selectedIndex = Math.max(0, Math.min(selectedIndex, filters.length - 1))
    }
    // `drag`: one of a drag's many changes, all one undo step (backend.endGesture() ends it).
    function edited(next, drag) { backend.setVideoFilters(sourceId, next, drag === true) }
    function change(values, drag) {
        if (!selectedFilter) return
        const next = filters.slice()
        next[selectedIndex] = Object.assign({}, selectedFilter, values)
        edited(next, drag)
    }
    function move(from, to) {
        if (to < 0 || to >= filters.length) return
        const next = filters.slice()
        next.splice(to, 0, next.splice(from, 1)[0])
        selectedIndex = to
        edited(next)
    }
    function addFilter(type) {
        const d = backend.videoFilterTypes.find(x => x.type === type)
        if (!d || filters.length >= 32) return
        const item = {id: Date.now().toString(36) + Math.random().toString(36).slice(2, 8), type: d.type, enabled: true}
        for (const p of d.parameters) item[p.key] = p.initial
        selectedIndex = filters.length
        edited(filters.concat([item]))
        // A key starts from the backdrop actually filmed.
        if (type === "chromaKey") backend.findKeyBackdrop(sourceId, item.id)
    }
    function removeFilter(index) {
        if (index < 0 || index >= filters.length) return
        const next = filters.slice()
        next.splice(index, 1)
        if (index < selectedIndex || selectedIndex >= next.length) selectedIndex = Math.max(0, selectedIndex - 1)
        edited(next)
    }
    function switchFilter(index, on) {
        if (index < 0 || index >= filters.length) return
        const next = filters.slice()
        next[index] = Object.assign({}, filters[index], {enabled: on})
        edited(next)
    }
    function shown(p) {
        if (!p.showIf || !selectedFilter) return true
        return Object.keys(p.showIf).every(k => String(selectedFilter[k]) === p.showIf[k])
    }
    // One column for the selected filter's labels, as wide as the longest, so
    // its fields all start in the same place.
    FontMetrics { id: labelMetrics; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
    readonly property real labelColumn: Math.ceil(Math.max(Theme.s4,
        ...(definition?.parameters ?? []).filter(p => p.kind !== "toggle" && !p.wheel).map(p => labelMetrics.advanceWidth(p.label)),
        (definition?.presets ?? []).length > 0 ? labelMetrics.advanceWidth(qsTr("Size")) : 0)) + Theme.s2
    // The selected filter's colour wheels, in order: {name, label, tip, initial, minimum, maximum}.
    readonly property var wheels: {
        const out = []
        for (const p of definition?.parameters ?? [])
            if (p.wheel?.axis === "level") out.push({name: p.wheel.name, label: p.label, tip: p.tip, initial: p.initial, minimum: p.minimum, maximum: p.maximum})
        return out
    }
    // Scopes beside the preview: on for a colour filter (onSelectedFilterChanged), then as left.
    property bool scopes: false
    property string scopeMode: "waveform"
    function fileName(path) { return path ? path.substring(path.lastIndexOf("/") + 1) : qsTr("None chosen") }

    Connections {
        target: backend
        function onItemsChanged() { if (sheet.visible) sheet.refresh() }
        function onVideoFiltersEditRequested(id) { sheet.showSource(id) }
    }
    // Files are read by the session after a change: look again for problems.
    // and what the filters cost against the frame there is.
    property var load: ({})
    Timer {
        interval: 500; repeat: true; running: sheet.visible
        onTriggered: {
            sheet.problems = backend.videoFilterProblems(sheet.sourceId)
            sheet.load = backend.videoFilterLoad(sheet.sourceId)
        }
    }

    // The source with its filters, live, while the sheet is open: every change shows at once.
    // One view per source, so a late picture of the last one is never shown for this one.
    property string viewId: ""
    property bool hasPicture: false
    // Clicking the preview picks the chroma key's backdrop colour there.
    property bool picking: false
    // The preview shows the studio being edited, the source over what is
    // behind it (how Light wrap is seen), rather than the source alone.
    property bool inStudio: false
    onInStudioChanged: { hasPicture = false; settle.restart() }
    // Light wrap needs what is behind: selecting one shows the studio. A
    // colour filter turns the scopes on. Only on coming to a filter: every edit
    // brings a new copy of the one already selected, and what was unticked stays so.
    property string filterSeen: ""
    onSelectedFilterChanged: {
        const id = selectedFilter?.id ?? ""
        if (id === filterSeen) return
        filterSeen = id
        if (selectedFilter?.type === "lightWrap") inStudio = true
        if (["colourWheels", "colour", "lut"].includes(selectedFilter?.type)) scopes = true
    }
    readonly property bool isKey: ["chromaKey", "colourKey", "lumaKey", "mask", "removeBackground"].some(t => filters.some(f => f.type === t && f.enabled))
    // With no key on there is no matte to see, whatever the box last said.
    readonly property bool showingMatte: isKey && matteCheck.checked && !inStudio
    onSelectedIndexChanged: picking = false
    function askForPicture() {
        if (!visible || sourceId === "" || pictureBox.width < 4 || pictureBox.height < 4) return
        const id = "video-filters-" + sourceId + (inStudio ? "-studio" : "")
        if (viewId !== id) { stopPicture(); viewId = id }
        const dpr = pictureBox.Window.window ? pictureBox.Window.window.devicePixelRatio : 1
        backend.requestView(viewId, inStudio ? "preview" : "source", inStudio ? "" : sourceId,
                            Math.round(pictureBox.width * dpr), Math.round(pictureBox.height * dpr), 30)
    }
    function stopPicture() {
        if (viewId !== "") backend.releaseView(viewId)
        viewId = ""
        hasPicture = false
    }
    Timer { id: settle; interval: 100; onTriggered: sheet.askForPicture() }
    onOpened: settle.restart()
    onSourceIdChanged: { hasPicture = false; settle.restart() }
    onClosed: stopPicture()
    Component.onDestruction: stopPicture()
    Connections {
        target: backend
        function onViewReady(id, image) {
            if (id === "" || id !== sheet.viewId) return
            picture.setImage(image)
            if (sheet.scopes) scope.setImage(image)
            sheet.hasPicture = true
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.s3
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Filters run from top to bottom on the source's picture, wherever it is shown.")
            color: Theme.textMuted
        }
        Label {
            objectName: "videoFilterProblems"
            Layout.fillWidth: true
            visible: sheet.problems.length > 0
            wrapMode: Text.WordWrap
            text: qsTr("Not running: %1").arg(sheet.problems.join("; "))
            color: Theme.danger
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.s4
            FilterChain {
                Layout.preferredWidth: Math.min(sheet.width * 0.34, Theme.wInspector - Theme.s5)
                Layout.fillHeight: true
                kind: "Video"
                filters: sheet.filters
                types: sheet.addable
                catalogue: backend.videoFilterTypes
                selectedIndex: sheet.selectedIndex
                most: 32
                copyable: true
                canPaste: backend.canPasteVideoFilters
                onSelected: (index) => sheet.selectedIndex = index
                onAdded: (type) => sheet.addFilter(type)
                onRemoved: (index) => sheet.removeFilter(index)
                onMoved: (from, to) => sheet.move(from, to)
                onSwitched: (index, on) => sheet.switchFilter(index, on)
                onCopied: (index) => backend.copyVideoFilters(sheet.sourceId, index >= 0 ? sheet.filters[index].id : "")
                onPasted: backend.pasteVideoFilters(sheet.sourceId)
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Theme.s3
                // The picture, and the scopes beside it when they are on.
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.round(sheet.height * 0.36)
                    spacing: Theme.s2
                    Rectangle {
                        id: pictureBox
                        objectName: "videoFiltersPreviewBox"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "black"
                        radius: Theme.rControl
                        clip: true
                        onWidthChanged: settle.restart()
                        onHeightChanged: settle.restart()
                        // Where the picture is clear: a grey check, so a dark subject is not taken for it.
                        Canvas {
                            anchors.fill: picture
                            visible: picture.visible && !sheet.showingMatte && !sheet.inStudio
                            onPaint: {
                                const g = getContext("2d"), n = 12
                                for (let y = 0; y < height; y += n)
                                    for (let x = 0; x < width; x += n) {
                                        g.fillStyle = ((x + y) / n) % 2 ? "#5a5a5a" : "#3c3c3c"
                                        g.fillRect(x, y, n, n)
                                    }
                            }
                            onWidthChanged: requestPaint()
                            onHeightChanged: requestPaint()
                        }
                        PreviewView {
                            id: picture
                            objectName: "videoFiltersPreview"
                            visible: sheet.hasPicture
                            anchors.centerIn: parent
                            readonly property real aspect: imageHeight > 0 ? imageWidth / imageHeight : 16 / 9
                            width: Math.min(parent.width, parent.height * aspect)
                            height: width / aspect
                            showAlpha: sheet.showingMatte
                            MouseArea {
                                objectName: "videoFiltersPickArea"
                                anchors.fill: parent
                                enabled: sheet.picking
                                cursorShape: sheet.picking ? Qt.CrossCursor : Qt.ArrowCursor
                                onClicked: (mouse) => {
                                    sheet.picking = false
                                    if (sheet.selectedFilter) backend.findKeyBackdrop(sheet.sourceId, sheet.selectedFilter.id, mouse.x / width, mouse.y / height)
                                }
                            }
                        }
                        Label {
                            anchors.centerIn: parent
                            width: parent.width - Theme.s4 * 2
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            visible: !sheet.hasPicture
                            text: qsTr("No picture from this source yet")
                            color: Theme.textMuted
                        }
                    }
                    ColumnLayout {
                        visible: sheet.scopes
                        Layout.fillHeight: true
                        Layout.preferredWidth: (parent.width - parent.spacing) / 2
                        Layout.fillWidth: false
                        spacing: Theme.s1
                        ComboBox {
                            objectName: "videoFiltersScopeMode"
                            Layout.fillWidth: true
                            model: [qsTr("Waveform"), qsTr("RGB parade"), qsTr("Vectorscope"), qsTr("Histogram")]
                            readonly property var values: ["waveform", "parade", "vectorscope", "histogram"]
                            currentIndex: Math.max(0, values.indexOf(sheet.scopeMode))
                            onActivated: sheet.scopeMode = values[currentIndex]
                            ToolTip.text: qsTr("Waveform: brightness across the picture, 0 black to 100 white. RGB parade: red, green and blue apart, level where they meet on something white or grey. Vectorscope: colour, round the circle for hue and outwards for saturation; faces sit on the dashed line. Histogram: how much of the picture is at each level")
                            Accessible.name: qsTr("Scope")
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            radius: Theme.rControl
                            color: Theme.windowBg
                            clip: true
                            ScopeView {
                                id: scope
                                objectName: "videoFiltersScope"
                                anchors.fill: parent
                                anchors.margins: Theme.s1
                                mode: sheet.scopeMode
                                backgroundColor: Theme.windowBg
                                gridColor: Theme.borderStrong
                                textColor: Theme.textMuted
                            }
                            Label {
                                anchors.centerIn: parent
                                visible: !scope.hasPicture
                                text: qsTr("No picture yet")
                                color: Theme.textMuted
                            }
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    CheckBox {
                        objectName: "videoFiltersInStudio"
                        text: qsTr("In the studio")
                        checked: sheet.inStudio
                        onClicked: sheet.inStudio = checked
                        ToolTip.text: qsTr("Show the studio being edited, with this source over what is behind it: where Light wrap can be seen. Off shows the source alone")
                    }
                    CheckBox {
                        objectName: "videoFiltersScopes"
                        text: qsTr("Scopes")
                        checked: sheet.scopes
                        onClicked: sheet.scopes = checked
                        ToolTip.text: qsTr("Waveform, RGB parade, vectorscope or histogram of the preview, beside it, to grade by")
                    }
                    CheckBox {
                        id: matteCheck
                        objectName: "videoFiltersShowMatte"
                        text: qsTr("Show matte")
                        visible: sheet.isKey && !sheet.inStudio
                        ToolTip.text: qsTr("See only what is kept (white) and taken out (black), here in this preview; the program is not touched")
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: sheet.picking
                        wrapMode: Text.WordWrap
                        text: qsTr("Click the backdrop in the preview")
                        color: Theme.accent
                    }
                }
                Label {
                    objectName: "videoFiltersLoad"
                    Layout.fillWidth: true
                    visible: (sheet.load.text ?? "") !== "" && !sheet.picking
                    wrapMode: Text.WordWrap
                    text: sheet.load.text ?? ""
                    color: sheet.load.level === "warn" ? Theme.warning : Theme.textMuted
                }
                ScrollView {
                    id: controls
                    objectName: "videoFiltersControls"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: availableWidth
                    ColumnLayout {
                        width: parent.width
                        spacing: Theme.s3
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            visible: sheet.selectedFilter === null
                            text: qsTr("Add a filter to get started")
                            font.bold: true
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: sheet.wheels.length === 0
                            wrapMode: Text.WordWrap
                            text: sheet.definition?.description ?? ""
                            color: Theme.textMuted
                        }
                        RowLayout {
                            visible: (sheet.definition?.presets ?? []).length > 0
                            Layout.fillWidth: true
                            spacing: 0
                            Label { text: qsTr("Size"); color: Theme.textSecondary; Layout.preferredWidth: sheet.labelColumn; elide: Text.ElideRight }
                            ComboBox {
                                objectName: "videoFilterPreset"
                                ToolTip.text: qsTr("A common size: sets the width and height")
                                Layout.fillWidth: true
                                model: [qsTr("Choose a size…")].concat((sheet.definition?.presets ?? []).map(p => p.label))
                                onActivated: if (currentIndex > 0) { sheet.change(sheet.definition.presets[currentIndex - 1].values); currentIndex = 0 }
                                Accessible.name: qsTr("Size preset")
                            }
                        }
                        // Colour wheels: lift, gamma, gain and offset side by side (two by two when narrow).
                        GridLayout {
                            id: wheelGrid
                            objectName: "videoFilterWheels"
                            visible: sheet.wheels.length > 0
                            // As big as fits above the sliders, so the whole grade is in view at once.
                            readonly property real most: Math.min(Theme.s6 * 5, Math.max(Theme.s6 * 2, controls.height - Theme.hControl * 2 - Theme.hRow * 2 - Theme.s3 * 2))
                            Layout.preferredWidth: Math.min(parent.width, most * columns + columnSpacing * (columns - 1))
                            Layout.alignment: Qt.AlignHCenter
                            columns: parent.width >= Theme.s6 * 3 * 4 ? 4 : 2  // the panel's width: the capped wheels limit the grid's own
                            columnSpacing: Theme.s3
                            rowSpacing: Theme.s3
                            Repeater {
                                model: sheet.wheels
                                delegate: ColourWheel {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 1
                                    Layout.maximumWidth: wheelGrid.most
                                    Layout.alignment: Qt.AlignHCenter | Qt.AlignTop
                                    name: modelData.name
                                    label: modelData.label
                                    tip: modelData.tip
                                    initialLevel: modelData.initial
                                    minimum: modelData.minimum
                                    maximum: modelData.maximum
                                    pointX: Number(sheet.selectedFilter?.[modelData.name + "X"] ?? 0)
                                    pointY: Number(sheet.selectedFilter?.[modelData.name + "Y"] ?? 0)
                                    level: Number(sheet.selectedFilter?.[modelData.name] ?? modelData.initial)
                                    onPointMoved: (x, y, drag) => sheet.change({[modelData.name + "X"]: x, [modelData.name + "Y"]: y}, drag)
                                    onLevelMoved: (value, drag) => sheet.change({[modelData.name]: value}, drag)
                                    onReset: sheet.change({[modelData.name + "X"]: 0, [modelData.name + "Y"]: 0, [modelData.name]: modelData.initial})
                                    onFinished: backend.endGesture()
                                }
                            }
                        }
                        // What goes with the wheels, in a row beneath them.
                        RowLayout {
                            visible: sheet.wheels.length > 0
                            Layout.fillWidth: true
                            spacing: Theme.s3
                            Repeater {
                                model: sheet.wheels.length > 0 ? sheet.definition.parameters.filter(p => p.kind === "number" && !p.wheel) : []
                                delegate: ColumnLayout {
                                    id: gradeSlider
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 1
                                    spacing: 0
                                    readonly property real value: Number(sheet.selectedFilter?.[modelData.key] ?? modelData.initial)
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { Layout.fillWidth: true; text: gradeSlider.modelData.label; color: Theme.textSecondary; elide: Text.ElideRight }
                                        ToolButton {
                                            objectName: "videoFilterReset_" + gradeSlider.modelData.key
                                            icon.name: "rotate-ccw"
                                            enabled: gradeSlider.value !== gradeSlider.modelData.initial
                                            onClicked: sheet.change({[gradeSlider.modelData.key]: gradeSlider.modelData.initial})
                                            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                                            ToolTip.text: qsTr("Put %1 back as it was").arg(gradeSlider.modelData.label)
                                            Accessible.name: ToolTip.text
                                        }
                                    }
                                    Slider {
                                        objectName: "videoFilter_" + gradeSlider.modelData.key
                                        Layout.fillWidth: true
                                        from: gradeSlider.modelData.minimum; to: gradeSlider.modelData.maximum
                                        stepSize: gradeSlider.modelData.step
                                        value: gradeSlider.value
                                        onMoved: sheet.change({[gradeSlider.modelData.key]: value}, true)
                                        onPressedChanged: if (!pressed) backend.endGesture()
                                        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                                        ToolTip.text: gradeSlider.modelData.tip || gradeSlider.modelData.label
                                        Accessible.name: gradeSlider.modelData.label
                                    }
                                }
                            }
                        }
                        Repeater {
                            model: sheet.wheels.length > 0 ? [] : sheet.definition?.parameters ?? []
                            delegate: ColumnLayout {
                                id: param
                                required property var modelData
                                readonly property var value: sheet.selectedFilter?.[modelData.key] ?? modelData.initial
                                Layout.fillWidth: true
                                visible: sheet.shown(modelData) && !modelData.wheel
                                FilterSlider {
                                    id: paramSlider
                                    objectName: "videoFilter_" + param.modelData.key
                                    visible: param.modelData.kind === "number"
                                    Layout.fillWidth: true
                                    label: param.modelData.label
                                    labelWidth: sheet.labelColumn
                                    tip: param.modelData.tip ?? ""
                                    suffix: param.modelData.unit ?? ""
                                    step: param.modelData.step ?? 1
                                    minimum: param.modelData.minimum ?? 0
                                    maximum: param.modelData.maximum ?? 1
                                    initial: Number(param.modelData.initial ?? 0)
                                    value: Number(param.value)
                                    onCommitted: (v) => sheet.change({[param.modelData.key]: Math.max(minimum, Math.min(maximum, v))}, dragging)
                                    onFinished: backend.endGesture()
                                }
                                RowLayout {
                                    visible: param.modelData.kind === "choice"
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Label { text: param.modelData.label; color: Theme.textSecondary; Layout.preferredWidth: sheet.labelColumn; elide: Text.ElideRight }
                                    ComboBox {
                                        objectName: "videoFilterChoice_" + param.modelData.key
                                        ToolTip.text: param.modelData.tip || param.modelData.label
                                        Layout.fillWidth: true
                                        model: param.modelData.options
                                        textRole: "label"
                                        valueRole: "value"
                                        currentIndex: Math.max(0, (param.modelData.options ?? []).findIndex(o => o.value === param.value))
                                        onActivated: {
                                            sheet.change({[param.modelData.key]: currentValue})
                                            // Another screen colour: find that backdrop.
                                            if (sheet.selectedFilter?.type === "chromaKey" && param.modelData.key === "keyType")
                                                backend.findKeyBackdrop(sheet.sourceId, sheet.selectedFilter.id)
                                        }
                                        Accessible.name: param.modelData.label
                                    }
                                }
                                RowLayout {
                                    visible: param.modelData.kind === "colour"
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Label { text: param.modelData.label; color: Theme.textSecondary; Layout.preferredWidth: sheet.labelColumn; elide: Text.ElideRight }
                                    ColourField {
                                        objectName: "videoFilterColour_" + param.modelData.key
                                        ToolTip.text: param.modelData.label
                                        Layout.fillWidth: true
                                        value: param.modelData.kind === "colour" ? String(param.value) : "#000000"
                                        onCommitted: (v) => sheet.change({[param.modelData.key]: v})
                                    }
                                }
                                RowLayout {
                                    visible: param.modelData.kind === "file"
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Label { text: param.modelData.label; color: Theme.textSecondary; Layout.preferredWidth: sheet.labelColumn; elide: Text.ElideRight }
                                    Label {
                                        Layout.fillWidth: true
                                        Layout.rightMargin: Theme.s2
                                        text: param.modelData.kind === "file" ? sheet.fileName(String(param.value)) : ""
                                        elide: Text.ElideMiddle
                                        ToolTip.text: String(param.value)
                                    }
                                    Button {
                                        objectName: "chooseVideoFilterFile_" + param.modelData.key
                                        ToolTip.text: qsTr("Choose the file")
                                        text: qsTr("Choose…")
                                        onClicked: backend.chooseVideoFilterFile(sheet.sourceId, sheet.selectedFilter.id, param.modelData.key)
                                    }
                                }
                                CheckBox {
                                    objectName: "videoFilterToggle_" + param.modelData.key
                                    visible: param.modelData.kind === "toggle"
                                    ToolTip.text: param.modelData.tip || param.modelData.label
                                    text: param.modelData.label
                                    checked: param.value === true
                                    onClicked: sheet.change({[param.modelData.key]: checked})
                                }
                            }
                        }
                        RowLayout {
                            visible: sheet.selectedFilter?.type === "chromaKey"
                            Layout.fillWidth: true
                            Button {
                                objectName: "findKeyBackdrop"
                                text: qsTr("Find backdrop")
                                ToolTip.text: qsTr("Find the screen's colour in the source's picture again, after the lighting changes")
                                onClicked: backend.findKeyBackdrop(sheet.sourceId, sheet.selectedFilter.id)
                            }
                            Button {
                                objectName: "pickKeyBackdrop"
                                text: qsTr("Pick from preview")
                                checkable: true
                                checked: sheet.picking
                                ToolTip.text: qsTr("Click a spot of the screen in the preview to use its colour")
                                onClicked: { sheet.picking = checked; if (checked) { matteCheck.checked = false; sheet.inStudio = false } }
                            }
                        }
                        Item { Layout.fillWidth: true; implicitHeight: Theme.s2 }
                    }
                }
            }
        }
    }
}
