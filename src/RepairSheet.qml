import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Help → Repair…: collections and profiles that no longer read, set aside;
// the window's layout put back; capture grants for sources that are gone,
// cleared. Each asks first, and nothing is deleted: what changes is kept in
// one dated folder.
Sheet {
    id: sheet
    helpTopic: "crashed"
    objectName: "repairSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 600)
    modal: true
    title: qsTr("Repair")
    standardButtons: Dialog.Close

    // The window resets what it keeps of the layout itself.
    signal layoutReset()
    property var report: ({ problems: [], collections: 0, profiles: 0, staleGrants: 0, folder: "" })
    function refresh() { report = backend.repairCheck() }
    onAboutToShow: { backend.beginRepair(); refresh() }

    // One question before each change.
    function ask(question, detail, action) {
        confirm.title = question
        confirm.detail = detail
        confirm.action = action
        confirm.open()
    }

    component Section: ColumnLayout {
        property alias heading: label.text
        property alias detail: body.text
        default property alias actions: actionRow.data
        property alias items: listed.data
        Layout.fillWidth: true
        spacing: Theme.s2
        SectionLabel { id: label }
        Label { id: body; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textSecondary }
        ColumnLayout { id: listed; Layout.fillWidth: true; spacing: Theme.s1; visible: children.length > 0 }
        RowLayout { id: actionRow; spacing: Theme.s2 }
    }

    contentItem: ColumnLayout {
        spacing: Theme.s4
        Section {
            heading: qsTr("STUDIO COLLECTIONS AND PROFILES")
            detail: sheet.report.problems.length === 0
                    ? qsTr("All %1 and %2 read.").arg(Theme.plural(sheet.report.collections, qsTr("collection"), qsTr("collections")))
                                                 .arg(Theme.plural(sheet.report.profiles, qsTr("profile"), qsTr("profiles")))
                    : qsTr("These do not read. Setting them aside moves them out of the way, into the folder below, so OmaBroadcast starts clean without them.")
            Button {
                objectName: "repairSetAside"
                ToolTip.text: qsTr("Move the files that do not read into the dated folder below")
                enabled: sheet.report.problems.length > 0
                text: qsTr("Set aside…")
                onClicked: sheet.ask(qsTr("Set these files aside?"),
                                     qsTr("They move into %1. Nothing is deleted; copy one back to use it again.").arg(sheet.report.folder),
                                     () => backend.setAsideUnreadable())
            }
            items: Repeater {
                model: sheet.report.problems
                delegate: ColumnLayout {
                    required property var modelData
                    objectName: "repairProblem"
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        Layout.fillWidth: true
                        text: (modelData.kind === "profile" ? qsTr("Profile “%1”") : qsTr("Collection “%1”")).arg(modelData.id)
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                    }
                    Label { Layout.fillWidth: true; text: modelData.why; color: Theme.warning; font.pixelSize: Theme.fsLabel; elide: Text.ElideRight }
                }
            }
        }
        Section {
            heading: qsTr("WINDOW LAYOUT")
            detail: qsTr("Put the docks, folded panels, preview, studio list, multiview and projectors back as they were at first. Recording, streaming and other settings stay.")
            Button {
                objectName: "repairResetLayout"
                ToolTip.text: qsTr("Reset the window's layout, keeping a copy of the old settings")
                text: qsTr("Reset layout…")
                onClicked: sheet.ask(qsTr("Reset the window layout?"),
                                     qsTr("The old settings are copied into %1 first.").arg(sheet.report.folder),
                                     () => { if (backend.resetWindowPreferences()) sheet.layoutReset() })
            }
        }
        Section {
            heading: qsTr("CAPTURE GRANTS")
            detail: sheet.report.staleGrants === 0
                    ? qsTr("Every saved screen or window choice belongs to a source that is still there.")
                    : qsTr("%1 belong to sources or collections that are gone.")
                          .arg(Theme.plural(sheet.report.staleGrants, qsTr("saved screen or window choice"), qsTr("saved screen or window choices")))
            Button {
                objectName: "repairClearGrants"
                ToolTip.text: qsTr("Forget the saved screen and window choices whose sources are gone")
                enabled: sheet.report.staleGrants > 0
                text: qsTr("Clear stale grants…")
                onClicked: sheet.ask(qsTr("Clear stale capture grants?"),
                                     qsTr("A copy goes into %1 first. Sources that are still there keep theirs.").arg(sheet.report.folder),
                                     () => backend.clearStaleGrants())
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textMuted
            font.pixelSize: Theme.fsLabel
            text: qsTr("Nothing is deleted. What is set aside goes into %1").arg(sheet.report.folder)
        }
    }

    Sheet {
        id: confirm
        objectName: "repairConfirm"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s5 * 2, 460)
        modal: true
        standardButtons: Dialog.Cancel | Dialog.Ok
        property string detail
        property var action: null
        onAccepted: { if (action) action(); action = null; sheet.refresh() }
        onRejected: action = null
        contentItem: Label { wrapMode: Text.Wrap; color: Theme.textSecondary; text: confirm.detail }
    }
}
