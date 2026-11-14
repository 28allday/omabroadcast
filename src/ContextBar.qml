import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Under the preview: the selected source at a glance, with the handful of
// things done to it most often during a show.
Rectangle {
    id: bar
    objectName: "contextBar"
    implicitHeight: Theme.hStatusBar + Theme.s2
    color: Theme.windowBg

    readonly property var row: backend.items.find(r => r.id === backend.selectedItem) ?? null
    readonly property var playback: backend.selectedMedia
    readonly property var slides: backend.selectedSlideshow
    readonly property string kind: row ? row.kind : ""
    readonly property bool mediaFile: kind === "media" && row && !row.network
    // The selected source's settings, for the quick edits below.
    readonly property var set: backend.selected.sourceId === (row ? row.sourceId : "") ? backend.selected.settings ?? ({}) : ({})
    readonly property var kindIcon: ({ display: "monitor", window: "app-window", region: "square", image: "image", colour: "palette",
                                       text: "type", device: "camera", media: "film", studio: "clapperboard", slideshow: "images",
                                       group: "group", "audio-input": "mic", "audio-output": "volume-2", "audio-application": "app-window",
                                       browser: "globe" })

    component BarButton: ToolButton {
        ToolTip.visible: hovered
        ToolTip.delay: Theme.tooltipDelay
        Accessible.name: ToolTip.text
    }

    Rectangle { anchors.top: parent.top; width: parent.width; height: Theme.hairline; color: Theme.border }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.s4
        anchors.rightMargin: Theme.s2
        spacing: Theme.s2

        Label {
            visible: !bar.row
            text: qsTr("No source selected")
            color: Theme.textMuted
            Layout.fillWidth: true
        }

        Icon {
            visible: !!bar.row
            name: bar.row && bar.row.network ? "smartphone" : bar.kindIcon[bar.kind] ?? "monitor"
            color: bar.row && bar.row.live ? Theme.textSecondary : Theme.textMuted
        }
        Label {
            objectName: "contextBarName"
            visible: !!bar.row
            text: bar.row ? bar.row.name : ""
            color: Theme.textPrimary
            elide: Text.ElideRight
            Layout.maximumWidth: implicitWidth
            Layout.fillWidth: true
        }
        Label {
            visible: !!bar.row && (bar.row.detail ?? "") !== ""
            text: bar.row ? bar.row.detail ?? "" : ""
            color: Theme.textMuted
            font.pixelSize: Theme.fsLabel
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Item { visible: !!bar.row && (bar.row.detail ?? "") === ""; Layout.fillWidth: true }

        // Media files: play or pause, and back to the start.
        BarButton {
            objectName: "contextMediaPlay"
            visible: bar.mediaFile
            enabled: bar.playback.state !== undefined && !bar.playback.closed
            icon.name: bar.playback.paused || bar.playback.state === "ended" ? "play" : "pause"
            ToolTip.text: bar.playback.paused || bar.playback.state === "ended" ? qsTr("Play") : qsTr("Pause")
            onClicked: {
                if (bar.playback.state === "ended") backend.controlMedia(bar.row.sourceId, "restart")
                else backend.controlMedia(bar.row.sourceId, "pause", bar.playback.paused ? 0 : 1)
            }
        }
        BarButton {
            visible: bar.mediaFile
            enabled: bar.playback.state !== undefined && !bar.playback.closed
            icon.name: "rotate-ccw"
            ToolTip.text: qsTr("Restart")
            onClicked: backend.controlMedia(bar.row.sourceId, "restart")
        }

        // Slideshows: step through, or hold.
        BarButton {
            visible: bar.kind === "slideshow"
            icon.name: "chevron-left"
            ToolTip.text: qsTr("Previous slide")
            onClicked: backend.controlSlideshow(bar.row.sourceId, "previous")
        }
        BarButton {
            objectName: "contextSlideshowPlay"
            visible: bar.kind === "slideshow"
            icon.name: bar.slides.paused ? "play" : "pause"
            ToolTip.text: bar.slides.paused ? qsTr("Play slideshow") : qsTr("Pause slideshow")
            onClicked: backend.controlSlideshow(bar.row.sourceId, "pause", bar.slides.paused ? 0 : 1)
        }
        BarButton {
            visible: bar.kind === "slideshow"
            icon.name: "chevron-right"
            ToolTip.text: qsTr("Next slide")
            onClicked: backend.controlSlideshow(bar.row.sourceId, "next")
        }

        // Colours, text and capture devices: the setting changed most, in place.
        ColourField {
            ToolTip.text: qsTr("The colour")
            objectName: "contextColour"
            visible: bar.kind === "colour"
            Layout.preferredWidth: Theme.s6 * 4
            value: bar.set.colour ?? "#000000"
            onCommitted: (value) => backend.setSourceSetting(bar.row.sourceId, "colour", value)
        }
        TextField {
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("The text to show; Enter applies it")
            objectName: "contextText"
            visible: bar.kind === "text" && !(bar.set.file ?? "")
            Layout.preferredWidth: Theme.s6 * 8
            text: bar.set.text ?? ""
            onAccepted: if (text !== (bar.set.text ?? "")) backend.setSourceSetting(bar.row.sourceId, "text", text)
            onEditingFinished: if (text !== (bar.set.text ?? "")) backend.setSourceSetting(bar.row.sourceId, "text", text)
            Accessible.name: qsTr("Text")
        }
        ComboBox {
            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: qsTr("Which camera or capture card it shows")
            objectName: "contextDevice"
            visible: bar.kind === "device"
            Layout.preferredWidth: Theme.s6 * 6
            readonly property var entries: {
                const plugged = backend.videoDevices.map(d => ({ name: d.name, label: d.description || d.name }))
                const node = bar.set.node ?? ""
                return node === "" || plugged.some(d => d.name === node) ? plugged
                       : [{ name: node, label: qsTr("%1 (not connected)").arg(node) }].concat(plugged)
            }
            model: entries
            textRole: "label"
            valueRole: "name"
            // A new list resets the box, and until it is made indexOfValue finds nothing.
            function followNode() { currentIndex = Qt.binding(() => indexOfValue(bar.set.node ?? "")) }
            onModelChanged: followNode()
            Component.onCompleted: followNode()
            onActivated: backend.setSourceSetting(bar.row.sourceId, "node", currentValue)
            Accessible.name: qsTr("Capture device")
        }

        // A page: load it again, or use it by hand.
        BarButton {
            objectName: "contextReloadPage"
            visible: bar.kind === "browser"
            icon.name: "refresh-cw"
            ToolTip.text: qsTr("Load the page again")
            onClicked: backend.reloadBrowser(bar.row.sourceId)
        }
        Button {
            ToolTip.text: qsTr("Open the page in a window to click, scroll and type in it")
            objectName: "contextInteract"
            visible: bar.kind === "browser"
            text: qsTr("Interact…")
            icon.name: "mouse-pointer-click"
            onClicked: backend.openInteract(bar.row.sourceId)
        }

        // Captures: pick what they show.
        Button {
            ToolTip.text: qsTr("Pick another display or window through the desktop's share dialog")
            objectName: "contextChooseCapture"
            visible: ["display", "window", "region"].includes(bar.kind)
            text: bar.kind === "window" ? qsTr("Choose window…") : qsTr("Choose display…")
            onClicked: backend.reopenSource(bar.row.sourceId)
        }

        BarButton {
            visible: !!bar.row && !["audio-input", "audio-output", "audio-application"].includes(bar.kind)
            icon.name: "maximize-2"
            ToolTip.text: qsTr("Edit transform…")
            onClicked: backend.editTransform(bar.row.id)
        }
        BarButton {
            objectName: "contextVisible"
            visible: !!bar.row
            icon.name: bar.row && bar.row.visible ? "eye" : "eye-off"
            ToolTip.text: bar.row && bar.row.visible ? qsTr("Hide") : qsTr("Show")
            onClicked: backend.setItemVisible(bar.row.id, !bar.row.visible)
        }
        BarButton {
            objectName: "contextLock"
            visible: !!bar.row
            icon.name: bar.row && bar.row.locked ? "lock" : "lock-open"
            ToolTip.text: bar.row && bar.row.locked ? qsTr("Unlock") : qsTr("Lock")
            onClicked: backend.setItemLocked(bar.row.id, !bar.row.locked)
        }
    }
}
