import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

Sheet {
    id: sheet
    helpTopic: "audio-filters"
    objectName: "audioFiltersSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: qsTr("Audio filters")
    width: Math.min(820, parent ? parent.width - Theme.s4 : 820)
    height: Math.min(600, parent ? parent.height - Theme.s4 : 600)
    standardButtons: Dialog.Close
    property var filters: []
    property int selectedIndex: 0
    readonly property var selectedFilter: filters[selectedIndex] ?? null
    // Undo can shorten the list from outside.
    onFiltersChanged: selectedIndex = Math.max(0, Math.min(selectedIndex, filters.length - 1))
    readonly property var definition: selectedFilter ? backend.audioFilterTypes.find(d => d.type === selectedFilter.type) : null
    signal edited(var filters)
    // One column for the selected filter's labels, so its fields line up.
    FontMetrics { id: labelMetrics; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl }
    readonly property real labelColumn: Math.ceil(Math.max(Theme.s4, ...(definition?.parameters ?? []).map(p => labelMetrics.advanceWidth(p.label)))) + Theme.s2

    function change(key, value) {
        if (!selectedFilter) return
        const next = filters.slice()
        next[selectedIndex] = Object.assign({}, selectedFilter)
        next[selectedIndex][key] = value
        edited(next)
    }
    function move(from, to) {
        if (to < 0 || to >= filters.length) return
        const next = filters.slice()
        const item = next.splice(from, 1)[0]
        next.splice(to, 0, item)
        selectedIndex = to
        edited(next)
    }
    function addFilter(type) {
        const d = backend.audioFilterTypes.find(x => x.type === type)
        if (!d || filters.length >= 16) return
        const item = {id: Date.now().toString(36) + Math.random().toString(36).slice(2), type: d.type, enabled: true}
        for (const p of d.parameters) item[p.key] = p.initial
        selectedIndex = filters.length
        edited(filters.concat([item]))
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
    contentItem: ColumnLayout {
        spacing: Theme.s3
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Filters run from top to bottom, before the channel fader, on both monitoring and output tracks.")
            color: Theme.textMuted
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.s4
            FilterChain {
                Layout.preferredWidth: sheet.width * 0.36
                Layout.fillHeight: true
                kind: "Audio"
                filters: sheet.filters
                types: backend.audioFilterTypes
                selectedIndex: sheet.selectedIndex
                most: 16
                onSelected: (index) => sheet.selectedIndex = index
                onAdded: (type) => sheet.addFilter(type)
                onRemoved: (index) => sheet.removeFilter(index)
                onMoved: (from, to) => sheet.move(from, to)
                onSwitched: (index, on) => sheet.switchFilter(index, on)
            }
            ScrollView {
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
                        text: sheet.definition?.label ?? qsTr("Add a filter to get started")
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: sheet.definition?.description ?? ""
                        color: Theme.textMuted
                    }
                    Repeater {
                        model: sheet.definition?.parameters ?? []
                        delegate: FilterSlider {
                            required property var modelData
                            Layout.fillWidth: true
                            objectName: "audioFilter_" + modelData.key
                            label: modelData.label
                            labelWidth: sheet.labelColumn
                            suffix: modelData.unit
                            step: modelData.step
                            minimum: modelData.minimum
                            maximum: modelData.maximum
                            initial: modelData.initial
                            value: sheet.selectedFilter?.[modelData.key] ?? modelData.initial
                            onCommitted: (v) => sheet.change(modelData.key, Math.max(minimum, Math.min(maximum, v)))
                        }
                    }
                    Item { Layout.fillWidth: true; implicitHeight: Theme.s2 }
                }
            }
        }
    }
}
