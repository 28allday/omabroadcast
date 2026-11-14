import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Help → Log: what the app has said this run, or in the run before, by
// level and words; copy what shows, or save the whole run to a file.
Sheet {
    id: sheet
    helpTopic: "crashed"
    objectName: "logSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 920)
    height: Math.min(parent.height - Theme.s5 * 2, 640)
    modal: true
    title: previous ? qsTr("Log of the run before") : qsTr("Log")
    standardButtons: Dialog.Close

    property bool previous: false
    property var rows: []
    property double seen: -1
    readonly property var levels: [qsTr("Everything"), qsTr("Information and up"), qsTr("Warnings and errors"), qsTr("Errors only")]
    readonly property var levelColours: [Theme.textMuted, Theme.textSecondary, Theme.warning, Theme.danger, Theme.danger]

    function showRun(previousRun) {
        previous = previousRun && backend.hasPreviousLog()
        open()
        reload()
    }
    function minimumLevel() { return Math.max(0, levelBox.currentIndex) }
    function reload() {
        const atEnd = list.atYEnd || list.count === 0
        rows = backend.logLines(previous, minimumLevel(), search.text)
        seen = backend.logSerial()
        if (atEnd) Qt.callLater(list.positionViewAtEnd)
    }
    onAboutToShow: seen = -1

    // New lines while it is open, about once a second.
    Timer {
        interval: 1000
        repeat: true
        running: sheet.visible && !sheet.previous
        onTriggered: if (backend.logSerial() !== sheet.seen) sheet.reload()
    }

    contentItem: ColumnLayout {
        spacing: Theme.s3
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s2
            ComboBox {
                id: runBox
                objectName: "logRun"
                ToolTip.text: qsTr("This run's log, or the one before it")
                visible: backend.hasPreviousLog()
                model: [qsTr("This run"), qsTr("The run before")]
                currentIndex: sheet.previous ? 1 : 0
                onActivated: (index) => { sheet.previous = index === 1; sheet.reload() }
                Accessible.name: qsTr("Which run")
            }
            ComboBox {
                id: levelBox
                objectName: "logLevel"
                ToolTip.text: qsTr("Which lines to show, by how serious they are")
                Layout.preferredWidth: Theme.wInspector * 0.7
                model: sheet.levels
                currentIndex: 1
                onActivated: sheet.reload()
                Accessible.name: qsTr("Level")
            }
            TextField {
                id: search
                objectName: "logSearch"
                ToolTip.text: qsTr("Show only lines with these words")
                Layout.fillWidth: true
                placeholderText: qsTr("Search")
                onTextChanged: sheet.reload()
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.windowBg
            border.color: Theme.border
            border.width: Theme.hairline
            radius: Theme.rControl
            ListView {
                id: list
                objectName: "logList"
                anchors.fill: parent
                anchors.margins: Theme.s2
                clip: true
                model: sheet.rows
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                delegate: RowLayout {
                    required property var modelData
                    width: ListView.view.width - Theme.s3
                    spacing: Theme.s2
                    Label {
                        Layout.alignment: Qt.AlignTop
                        text: modelData.time
                        color: Theme.textMuted
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsLabel
                    }
                    Label {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: 70
                        text: ["debug", "info", "warning", "error", "fatal"][modelData.level] ?? ""
                        color: sheet.levelColours[modelData.level] ?? Theme.textSecondary
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsLabel
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData.text
                        color: modelData.level >= 2 ? sheet.levelColours[modelData.level] : Theme.textPrimary
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsLabel
                        wrapMode: Text.WrapAnywhere
                        textFormat: Text.PlainText
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: list.count === 0
                color: Theme.textMuted
                text: search.text !== "" || levelBox.currentIndex > 0 ? qsTr("No lines match.") : qsTr("Nothing logged yet.")
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s2
            Label {
                Layout.fillWidth: true
                color: Theme.textMuted
                font.pixelSize: Theme.fsLabel
                text: Theme.plural(list.count, qsTr("line"), qsTr("lines"))
            }
            Button {
                objectName: "logCopy"
                ToolTip.text: qsTr("Copy the lines shown, to paste into a bug report")
                text: qsTr("Copy")
                icon.name: "copy"
                enabled: list.count > 0
                onClicked: backend.copyLog(sheet.previous, sheet.minimumLevel(), search.text)
            }
            Button {
                objectName: "logSave"
                ToolTip.text: qsTr("Save this run's whole log to a file")
                text: qsTr("Save log…")
                icon.name: "file-text"
                onClicked: backend.saveLog(sheet.previous)
            }
        }
    }
}
