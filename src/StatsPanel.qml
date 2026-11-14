import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// The stats dock's body: what the machine, the frame loop and each output
// are doing. The numbers come about once a second while the dock is watched.
ColumnLayout {
    id: panel
    readonly property var stats: backend.systemStats
    spacing: Theme.s1

    function percent(v) { return v === undefined || v < 0 ? qsTr("measuring…") : v.toFixed(1) + " %" }
    function ms(v) { return v === undefined || v < 0 ? "—" : qsTr("%1 ms").arg(v.toFixed(1)) }
    function minutesLeft() {
        if (!stats.recording) return qsTr("not recording")
        if (stats.minutesLeft === undefined || stats.minutesLeft < 0) return qsTr("measuring…")
        const m = Math.floor(stats.minutesLeft)
        return m >= 120 ? qsTr("%1 h %2 min").arg(Math.floor(m / 60)).arg(m % 60) : qsTr("%1 min").arg(m)
    }

    // One reading: what it is, and the number, lined up in a column.
    component Fact: RowLayout {
        property alias label: name.text
        property alias value: number.text
        property bool warn: false
        Layout.fillWidth: true
        spacing: Theme.s3
        Label {
            id: name
            Layout.preferredWidth: 132
            color: Theme.textMuted
            font.pixelSize: Theme.fsLabel
            elide: Text.ElideRight
        }
        Label {
            id: number
            Layout.fillWidth: true
            color: warn ? Theme.warning : Theme.textPrimary
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            elide: Text.ElideRight
        }
    }

    SectionLabel { text: qsTr("MACHINE") }
    Fact { objectName: "statsProcessCpu"; label: qsTr("CPU, this app"); value: panel.percent(panel.stats.processCpu) }
    Fact { objectName: "statsSystemCpu"; label: qsTr("CPU, all"); value: panel.percent(panel.stats.systemCpu)
           + (panel.stats.cpus > 0 ? "  " + qsTr("of %1 CPUs").arg(panel.stats.cpus) : "") }
    Repeater {
        model: panel.stats.gpus ?? []
        delegate: Fact {
            required property var modelData
            label: modelData.name
            value: modelData.busy < 0 ? qsTr("not available") : modelData.busy + " %"
        }
    }
    Fact { visible: (panel.stats.gpus ?? []).length === 0; label: qsTr("Graphics"); value: qsTr("not available") }
    Fact { objectName: "statsMemory"; label: qsTr("Memory, this app"); value: panel.stats.resident || "—" }

    SectionLabel { text: qsTr("RECORDINGS FOLDER") }
    Fact { objectName: "statsDiskFree"; label: qsTr("Disk free"); value: panel.stats.free || qsTr("not available")
           warn: panel.stats.freeBytes >= 0 && panel.stats.freeBytes < 2 * 1024 * 1024 * 1024 }
    Fact { label: qsTr("Time left"); value: panel.minutesLeft(); warn: !!panel.stats.recording && panel.stats.minutesLeft >= 0 && panel.stats.minutesLeft < 10 }

    SectionLabel { text: qsTr("VIDEO") }
    Fact { objectName: "statsFps"; label: qsTr("Frames per second"); value: (panel.stats.fps ?? 0).toFixed(1) }
    Fact { objectName: "statsRender"; label: qsTr("Render time"); value: panel.ms(panel.stats.renderMs) + "  " + qsTr("worst %1").arg(panel.ms(panel.stats.renderWorstMs)) }
    Fact { objectName: "statsLate"; label: qsTr("Late frames"); value: String(panel.stats.lateTicks ?? 0); warn: (panel.stats.lateTicks ?? 0) > 0 }
    Fact { label: qsTr("Engine"); value: panel.stats.engine ? (panel.stats.engineWhy ? panel.stats.engine + " — " + panel.stats.engineWhy : panel.stats.engine) : "—" }

    SectionLabel { text: qsTr("OUTPUTS") }
    Label {
        visible: (panel.stats.outputs ?? []).length === 0
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textMuted
        font.pixelSize: Theme.fsLabel
        text: qsTr("Nothing is recording, streaming or running.")
    }
    Repeater {
        model: panel.stats.outputs ?? []
        delegate: ColumnLayout {
            required property var modelData
            Layout.fillWidth: true
            spacing: 0
            Label { text: modelData.name; color: Theme.textSecondary; font.pixelSize: Theme.fsLabel; font.weight: Theme.wHeading
                    elide: Text.ElideRight; Layout.fillWidth: true }
            Fact { visible: (modelData.encoder ?? "") !== ""; label: qsTr("Encoder")
                   value: modelData.encoder + (modelData.hardware ? qsTr(" — hardware") : qsTr(" — processor")) }
            Fact { visible: modelData.encodeMs >= 0; label: qsTr("Encode time"); value: panel.ms(modelData.encodeMs) }
            Fact { visible: modelData.lagMs >= 0; label: qsTr("Sending behind"); value: panel.ms(modelData.lagMs) }
            Fact { label: qsTr("Dropped frames"); value: qsTr("%1 of %2").arg(modelData.dropped).arg(modelData.dropped + modelData.frames)
                   warn: modelData.dropped > 0 }
        }
    }
}
