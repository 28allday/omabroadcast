import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// The guide, on F1 and Help ▸ Guide. Three kinds of page in one list:
// tasks a beginner can follow step by step (the coach walks them through on
// the real window), the parts of the window for looking things up, and what
// to do when something goes wrong. Every part keeps the basics first and the
// detail an experienced user wants under "In more depth".
//
// The words live in HelpTopics.qml so that a website can be made from them
// later without touching this layout.
Sheet {
    id: root
    objectName: "helpGuide"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 1040)
    height: Math.min(parent.height - Theme.s5 * 2, 720)
    modal: true
    title: qsTr("OmaBroadcast guide")
    standardButtons: Dialog.Close

    // Asked of the window: walk through a task, or point at a part of it.
    signal startTask(string key)
    signal showPart(string target)
    signal keysRequested()

    HelpTopics { id: content }
    readonly property var topics: content.topics

    property string query: ""
    property string currentKey: content.topics[0].key
    readonly property var topic: content.topics.find(t => t.key === currentKey) ?? content.topics[0]

    function topicOf(key) { return content.topics.find(t => t.key === key) ?? null }
    function showTopic(key) {
        query = ""
        if (content.topics.some(t => t.key === key)) currentKey = key
        open()
    }
    onOpened: search.forceActiveFocus()

    // A topic matches when every word typed is somewhere in it.
    function matches(t, q) {
        if (q === "") return true
        const hay = [t.name, t.summary, t.reach ?? "", t.also ?? "",
                     (t.points ?? []).join(" "), (t.more ?? []).join(" "),
                     (t.steps ?? []).map(s => s.title + " " + s.text).join(" "), (t.tips ?? []).join(" "),
                     (t.tables ?? []).map(b => [b.title].concat(b.columns, ...b.rows).join(" ")).join(" ")].join(" ").toLowerCase()
        return q.toLowerCase().split(/\s+/).every(w => hay.indexOf(w) >= 0)
    }
    readonly property var groups: [
        { kind: "task", title: qsTr("LEARN BY DOING") },
        { kind: "part", title: qsTr("PARTS OF THE WINDOW") },
        { kind: "fix", title: qsTr("WHEN SOMETHING GOES WRONG") },
        { kind: "faq", title: qsTr("QUESTIONS PEOPLE ASK") }
    ]
    readonly property var shown: {
        const out = []
        for (const g of groups) {
            const items = content.topics.filter(t => t.kind === g.kind && matches(t, query))
            if (items.length === 0) continue
            out.push({ heading: g.title })
            for (const t of items) out.push(t)
        }
        return out
    }
    // The current topic no longer listed: the first whose own name has a word
    // starting with each word typed ("phone" is not "microphone"), else the first listed.
    onShownChanged: if (query !== "" && !shown.some(t => t.key === currentKey)) {
        const words = query.toLowerCase().split(/\s+/)
        const named = t => { const n = t.name.toLowerCase().split(/[\s.,:;?!()"“”/·-]+/); return words.every(w => n.some(x => x.startsWith(w))) }
        const first = shown.find(t => t.key !== undefined && named(t))
                   ?? shown.find(t => t.key !== undefined)
        if (first) currentKey = first.key
    }

    RowLayout {
        anchors.fill: parent
        spacing: Theme.s4

        ColumnLayout {
            Layout.preferredWidth: 270
            Layout.fillHeight: true
            spacing: Theme.s2

            TextField {
                id: search
                objectName: "helpSearch"
                ToolTip.text: qsTr("Find tasks, parts of the window and problems by any word in them")
                Layout.fillWidth: true
                placeholderText: qsTr("Search the guide")
                text: root.query
                onTextEdited: root.query = text
                leftPadding: Theme.s2 * 2 + Theme.szIcon
                Icon { name: "search"; color: Theme.textMuted; anchors.verticalCenter: parent.verticalCenter; x: Theme.s2 }
                Keys.onDownPressed: list.forceActiveFocus()
            }

            ListView {
                id: list
                objectName: "helpTopics"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: root.shown
                boundsBehavior: Flickable.StopAtBounds
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Guide topics")
                delegate: Loader {
                    required property var modelData
                    width: ListView.view.width
                    sourceComponent: modelData.heading !== undefined ? headingRow : topicRow
                    property var entry: modelData
                }
                Label {
                    anchors.centerIn: parent
                    visible: root.shown.length === 0
                    text: qsTr("Nothing in the guide matches")
                    color: Theme.textMuted
                }
            }
            Component {
                id: headingRow
                SectionLabel { text: parent.entry.heading; topPadding: Theme.s3; bottomPadding: Theme.s1 }
            }
            Component {
                id: topicRow
                ItemDelegate {
                    readonly property var t: parent.entry
                    objectName: "helpTopic_" + t.key
                    highlighted: root.currentKey === t.key
                    Accessible.name: t.name
                    contentItem: RowLayout {
                        spacing: Theme.s2
                        Icon { name: t.icon; color: highlighted ? Theme.accent : Theme.textMuted }
                        Label { text: t.name; Layout.fillWidth: true; elide: Text.ElideRight
                                color: highlighted ? Theme.textPrimary : t.draft ? Theme.textMuted : Theme.textSecondary }
                        Label { visible: t.kind === "task"; text: t.level ?? ""; color: Theme.textMuted; font.pixelSize: Theme.fsCaption }
                    }
                    onClicked: root.currentKey = t.key
                }
            }
        }

        Rectangle { Layout.fillHeight: true; implicitWidth: Theme.hairline; color: Theme.border }

        ScrollView {
            id: detailScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: detailScroll.availableWidth - Theme.s3
                spacing: Theme.s2

                RowLayout {
                    spacing: Theme.s2
                    Icon { name: root.topic.icon; color: Theme.accent; size: 20 }
                    Label { objectName: "helpTopicTitle"; text: root.topic.name; color: Theme.textPrimary
                            font.pixelSize: Theme.fsTitle + 3; font.weight: Theme.wHeading }
                }
                Label {
                    visible: root.topic.kind === "task"
                    text: [root.topic.level ?? "", root.topic.time ?? ""].filter(s => s !== "").join("  ·  ")
                    color: Theme.textMuted
                }
                Label {
                    objectName: "helpTopicSummary"
                    Layout.fillWidth: true
                    text: root.topic.summary
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fsSection
                    wrapMode: Text.WordWrap
                    lineHeight: 1.15
                }

                Label {
                    visible: root.topic.draft === true
                    Layout.topMargin: Theme.s3
                    text: qsTr("This page is still being written.")
                    color: Theme.textMuted
                }

                // A task: the steps, and a button to be walked through them.
                ColumnLayout {
                    visible: root.topic.kind === "task" && root.topic.draft !== true
                    Layout.fillWidth: true
                    spacing: Theme.s2
                    SectionLabel { text: qsTr("THE STEPS"); Layout.topMargin: Theme.s3 }
                    Repeater {
                        model: root.topic.kind === "task" ? (root.topic.steps ?? []) : []
                        RowLayout {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            spacing: Theme.s3
                            Rectangle {
                                Layout.alignment: Qt.AlignTop
                                implicitWidth: 22; implicitHeight: 22; radius: 11
                                color: Theme.controlBg; border.color: Theme.border
                                Label { anchors.centerIn: parent; text: index + 1; color: Theme.textSecondary; font.pixelSize: Theme.fsCaption }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label { text: modelData.title; color: Theme.textPrimary; font.weight: Theme.wHeading
                                        Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                Label { text: modelData.text; color: Theme.textSecondary
                                        Layout.fillWidth: true; wrapMode: Text.WordWrap }
                            }
                        }
                    }
                    Button {
                        objectName: "helpStartTask"
                        ToolTip.text: qsTr("Close the guide and follow the steps on the window itself")
                        Layout.topMargin: Theme.s3
                        highlighted: true
                        icon.name: "play"
                        text: qsTr("Walk me through it")
                        onClicked: { const key = root.topic.key; root.close(); root.startTask(key) }
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pixelSize: Theme.fsCaption
                        text: qsTr("The guide steps aside and a small card points at each control in turn. It moves on by itself when it can tell you have done a step; otherwise press Next.")
                    }
                    SectionLabel { visible: (root.topic.tips ?? []).length > 0; text: qsTr("GOOD TO KNOW"); Layout.topMargin: Theme.s4 }
                    Repeater {
                        model: root.topic.kind === "task" ? (root.topic.tips ?? []) : []
                        Bullet { required property string modelData; text: modelData }
                    }
                }

                // A part of the window, or a problem: what to know, then more depth.
                ColumnLayout {
                    visible: root.topic.kind !== "task" && root.topic.draft !== true
                    Layout.fillWidth: true
                    spacing: Theme.s2
                    SectionLabel { visible: (root.topic.reach ?? "") !== ""; text: qsTr("WHERE IT IS"); Layout.topMargin: Theme.s3 }
                    Label { visible: (root.topic.reach ?? "") !== ""; Layout.fillWidth: true; text: root.topic.reach ?? ""
                            color: Theme.textPrimary; wrapMode: Text.WordWrap }

                    SectionLabel {
                        text: root.topic.kind === "fix" ? qsTr("TRY THESE, IN ORDER") : root.topic.kind === "faq" ? qsTr("WHAT TO KNOW") : qsTr("THE BASICS")
                        Layout.topMargin: Theme.s3
                    }
                    Repeater {
                        model: root.topic.points ?? []
                        Bullet { required property string modelData; text: modelData }
                    }
                    // Figures, as a table: a title, a heading row, then the rows.
                    Repeater {
                        model: root.topic.tables ?? []
                        ColumnLayout {
                            id: table
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.s3
                            spacing: Theme.s1
                            Label { Layout.fillWidth: true; text: table.modelData.title; color: Theme.textPrimary; font.weight: Theme.wHeading; wrapMode: Text.WordWrap }
                            GridLayout {
                                objectName: "helpTable"
                                Layout.fillWidth: true
                                columns: table.modelData.columns.length
                                columnSpacing: Theme.s3
                                rowSpacing: Theme.s1
                                Repeater {
                                    model: table.modelData.columns.concat(...table.modelData.rows)
                                    Label {
                                        required property string modelData
                                        required property int index
                                        readonly property bool heading: index < table.modelData.columns.length
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: index % table.modelData.columns.length === 0 ? 1 : 2
                                        text: modelData
                                        color: heading ? Theme.textMuted : Theme.textSecondary
                                        font.pixelSize: heading ? Theme.fsCaption : Theme.fsLabel
                                        font.weight: heading ? Theme.wHeading : Font.Normal
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }
                    }

                    Button {
                        objectName: "helpMoreToggle"
                        ToolTip.text: qsTr("Details for when you know the basics")
                        visible: (root.topic.more ?? []).length > 0
                        Layout.topMargin: Theme.s3
                        flat: true
                        checkable: true
                        icon.name: checked ? "chevron-down" : "chevron-right"
                        text: qsTr("In more depth")
                        // Each topic opens closed: a beginner should not have to scroll past it.
                        Connections { target: root; function onCurrentKeyChanged() { moreToggle.checked = false } }
                        id: moreToggle
                    }
                    Repeater {
                        model: moreToggle.checked ? (root.topic.more ?? []) : []
                        Bullet { required property string modelData; text: modelData }
                    }
                }

                SectionLabel { visible: (root.topic.related ?? []).length > 0; text: qsTr("SEE ALSO"); Layout.topMargin: Theme.s4 }
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.s2
                    Repeater {
                        model: (root.topic.related ?? []).map(k => content.topics.find(t => t.key === k)).filter(t => t !== undefined)
                        Button {
                            required property var modelData
                            flat: true
                            ToolTip.text: qsTr("Open this page of the guide")
                            icon.name: modelData.icon
                            text: modelData.name
                            onClicked: root.currentKey = modelData.key
                        }
                    }
                }

                RowLayout {
                    Layout.topMargin: Theme.s4
                    spacing: Theme.s2
                    Button {
                        objectName: "helpShowMe"
                        ToolTip.text: qsTr("Close the guide and point at this part of the window")
                        visible: root.topic.kind !== "task" && (root.topic.target ?? "") !== ""
                        icon.name: "crosshair"
                        text: qsTr("Show me where")
                        onClicked: { const target = root.topic.target; root.close(); root.showPart(target) }
                    }
                    Button {
                        objectName: "helpAllKeys"
                        ToolTip.text: qsTr("Every keyboard shortcut on one page")
                        text: qsTr("All keyboard shortcuts")
                        icon.name: "keyboard"
                        flat: true
                        onClicked: { root.close(); root.keysRequested() }
                    }
                }
            }
        }
    }

    component Bullet: RowLayout {
        property alias text: line.text
        Layout.fillWidth: true
        spacing: Theme.s2
        Label { text: "•"; color: Theme.accent; Layout.alignment: Qt.AlignTop }
        Label { id: line; Layout.fillWidth: true; color: Theme.textPrimary; wrapMode: Text.WordWrap; lineHeight: 1.1 }
    }
}
