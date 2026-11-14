import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

Sheet {
    id: sheet
    helpTopic: "sources"
    objectName: "regionSelectionSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: qsTr("Select display region")
    width: Math.min(960, parent ? parent.width - Theme.s4 : 960)
    height: Math.min(700, parent ? parent.height - Theme.s4 : 700)
    property string sourceId: ""
    property int fullWidth: 1
    property int fullHeight: 1
    property int leftPixel: 0
    property int topPixel: 0
    property int rightPixel: 0
    property int bottomPixel: 0
    readonly property bool hasRegion: rightPixel > leftPixel && bottomPixel > topPixel

    function showCapture(id, image, width, height) {
        if (width <= 0 || height <= 0) return
        sourceId = id
        fullWidth = width
        fullHeight = height
        leftPixel = topPixel = rightPixel = bottomPixel = 0
        picture.setImage(image)
        open()
    }
    onAccepted: if (hasRegion) backend.setCaptureRegion(sourceId, leftPixel, topPixel, rightPixel-leftPixel, bottomPixel-topPixel)
    footer: DialogButtonBox {
        Button { text: qsTr("Cancel"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole; ToolTip.text: qsTr("Keep the region as it was") }
        Button {
            ToolTip.text: qsTr("Capture the rectangle you drew")
            objectName: "applyDrawnRegion"
            text: qsTr("Use region")
            enabled: sheet.hasRegion && !drag.pressed
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
    }
    contentItem: ColumnLayout {
        spacing: Theme.s2
        Label {
            Layout.fillWidth: true
            text: qsTr("Drag a rectangle on this display snapshot. The live crop changes when you choose Use region.")
            wrapMode: Text.Wrap
            color: Theme.textSecondary
        }
        Item {
            id: area
            Layout.fillWidth: true
            Layout.fillHeight: true
            PreviewView {
                id: picture
                anchors.centerIn: parent
                width: Math.min(area.width, area.height * sheet.fullWidth / sheet.fullHeight)
                height: width * sheet.fullHeight / sheet.fullWidth
                Rectangle {
                    anchors.fill: parent
                    color: "#66000000"
                }
                // The crop outline uses captured pixels; the display's desktop
                // scale and the preview's size do not affect saved coordinates.
                Rectangle {
                    visible: sheet.hasRegion
                    x: sheet.leftPixel / sheet.fullWidth * picture.width
                    y: sheet.topPixel / sheet.fullHeight * picture.height
                    width: (sheet.rightPixel-sheet.leftPixel) / sheet.fullWidth * picture.width
                    height: (sheet.bottomPixel-sheet.topPixel) / sheet.fullHeight * picture.height
                    color: Theme.withAlpha(Theme.accent, 0.12)
                    border.color: Theme.accent
                    border.width: 2
                }
                MouseArea {
                    id: drag
                    objectName: "captureRegionDrag"
                    anchors.fill: parent
                    cursorShape: Qt.CrossCursor
                    property int startX: 0
                    property int startY: 0
                    function pixelX(x) { return Math.max(0, Math.min(sheet.fullWidth, Math.round(x / width * sheet.fullWidth))) }
                    function pixelY(y) { return Math.max(0, Math.min(sheet.fullHeight, Math.round(y / height * sheet.fullHeight))) }
                    function moveSelection(x, y) {
                        const px = pixelX(x), py = pixelY(y)
                        sheet.leftPixel = Math.min(startX, px)
                        sheet.rightPixel = Math.max(startX, px)
                        sheet.topPixel = Math.min(startY, py)
                        sheet.bottomPixel = Math.max(startY, py)
                    }
                    onPressed: (mouse) => {
                        startX = pixelX(mouse.x); startY = pixelY(mouse.y)
                        moveSelection(mouse.x, mouse.y)
                    }
                    onPositionChanged: (mouse) => { if (pressed) moveSelection(mouse.x, mouse.y) }
                    onReleased: (mouse) => moveSelection(mouse.x, mouse.y)
                    onCanceled: { sheet.rightPixel = sheet.leftPixel; sheet.bottomPixel = sheet.topPixel }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            text: sheet.hasRegion ? qsTr("X %1 · Y %2 · %3 × %4 pixels").arg(sheet.leftPixel).arg(sheet.topPixel)
                .arg(sheet.rightPixel-sheet.leftPixel).arg(sheet.bottomPixel-sheet.topPixel)
                : qsTr("Display: %1 × %2 pixels").arg(sheet.fullWidth).arg(sheet.fullHeight)
            color: Theme.textSecondary
        }
    }
}
