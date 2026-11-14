import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

Sheet {
    id: sheet
    helpTopic: "sources"
    objectName: "directCaptureSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(660, parent ? parent.width - Theme.s4 : 660)
    height: Math.min(460, parent ? parent.height - Theme.s4 : 460)
    title: windowSource ? qsTr("Direct window capture") : qsTr("Direct display capture")
    standardButtons: Dialog.Close
    property string sourceId: ""
    property bool windowSource: false
    property var generation: 0
    property var catalog: ({ ready: false, targets: [] })
    property string selectedKey: ""
    readonly property bool supported: windowSource ? catalog.windows === true : catalog.displays === true
    readonly property var targets: (catalog.targets ?? []).filter(t => t.window === windowSource)
    readonly property var selectedTarget: targets.find(t => t.key === selectedKey) ?? null

    function showSource(id, window) {
        sourceId = id; windowSource = window
        generation = backend.collectionGeneration(); selectedKey = ""
        refresh(); open()
    }
    function refresh() {
        if (generation !== backend.collectionGeneration() || backend.captureSourceKind(sourceId) === "") { close(); return }
        catalog = backend.directCaptureCatalog()
    }
    Timer { interval: 250; repeat: true; running: sheet.visible; onTriggered: sheet.refresh() }
    Connections { target: backend; function onItemsChanged() { if (sheet.visible) sheet.refresh() } }
    contentItem: ColumnLayout {
        spacing: Theme.s3
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            text: !sheet.catalog.ready ? qsTr("Finding capture targets…") : !sheet.supported
                ? qsTr("Direct capture is unavailable on this compositor. Use the desktop picker.")
                : sheet.windowSource ? qsTr("Choose a window, or follow whichever window has focus. A saved window is restored only when its application and title match uniquely.")
                : qsTr("Choose a display. Its saved connector name is used when it reconnects.")
        }
        ListView {
            id: list
            objectName: "directCaptureTargets"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            enabled: sheet.supported
            model: sheet.targets
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                required property var modelData
                width: list.width
                height: Theme.hRow + Theme.s3
                highlighted: modelData.key === sheet.selectedKey
                onClicked: sheet.selectedKey = modelData.key
                contentItem: ColumnLayout {
                    spacing: 0
                    Label { Layout.fillWidth: true; text: modelData.name; textFormat: Text.PlainText; elide: Text.ElideRight; color: Theme.textPrimary }
                    Label { Layout.fillWidth: true; text: modelData.window ? modelData.appId : modelData.key.slice(7); textFormat: Text.PlainText; elide: Text.ElideRight; color: Theme.textMuted; font.pixelSize: Theme.fsCaption }
                }
            }
        }
        RowLayout {
            Button {
                ToolTip.text: qsTr("Choose through the desktop's share dialog instead")
                objectName: "directCapturePortal"
                text: qsTr("Desktop picker…")
                onClicked: { const id = sheet.sourceId; sheet.close(); backend.usePortalCapture(id) }
            }
            Item { Layout.fillWidth: true }
            Button {
                ToolTip.text: qsTr("Always capture whichever window has focus")
                objectName: "directCaptureFocused"
                visible: sheet.windowSource
                enabled: sheet.supported
                text: qsTr("Follow focus")
                onClicked: if (backend.setDirectCaptureTarget(sheet.sourceId, {}, true)) sheet.close()
            }
            Button {
                ToolTip.text: qsTr("Capture the selected display or window")
                objectName: "directCaptureUse"
                text: qsTr("Use selected")
                enabled: sheet.supported && sheet.selectedTarget !== null
                onClicked: if (backend.setDirectCaptureTarget(sheet.sourceId, sheet.selectedTarget)) sheet.close()
            }
        }
    }
}
