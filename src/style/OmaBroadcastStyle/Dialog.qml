import QtQuick
import QtQuick.Templates as T
import OmaBroadcast 1.0

// A raised card over a dimmed window: title row, body, button row on the right.
T.Dialog {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding,
                            implicitHeaderWidth,
                            implicitFooterWidth)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding
                             + (implicitHeaderHeight > 0 ? implicitHeaderHeight + spacing : 0)
                             + (implicitFooterHeight > 0 ? implicitFooterHeight + spacing : 0))

    padding: Theme.s4
    topPadding: Theme.s2
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsControl

    background: Rectangle {
        color: Theme.panelBg
        radius: Theme.rCard
        border.width: Theme.hairline
        border.color: Theme.borderStrong
    }

    // A sheet with a page in the guide (Sheet.helpTopic) shows a ? beside its title.
    readonly property string helpKey: control.helpTopic ?? ""
    header: Item {
        visible: control.title !== ""
        implicitWidth: titleText.implicitWidth + (helpButton.visible ? helpButton.implicitWidth : 0)
        implicitHeight: Math.max(titleText.implicitHeight, helpButton.visible ? helpButton.implicitHeight + Theme.s2 : 0)
        Text {
            id: titleText
            anchors.left: parent.left
            anchors.right: helpButton.visible ? helpButton.left : parent.right
            text: control.title
            textFormat: Text.PlainText
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsSection
            font.weight: Theme.wHeading
            color: Theme.textPrimary
            leftPadding: Theme.s4
            rightPadding: Theme.s4
            topPadding: Theme.s4
            bottomPadding: Theme.s2
        }
        ToolButton {
            id: helpButton
            objectName: "sheetHelpButton"
            visible: control.helpKey !== ""
            anchors.right: parent.right
            anchors.rightMargin: Theme.s2
            anchors.top: parent.top
            anchors.topMargin: Theme.s2
            icon.name: "circle-question-mark"
            onClicked: Sheets.helpRequested(control.helpKey)
            T.ToolTip.visible: hovered
            T.ToolTip.delay: Theme.tooltipDelay
            T.ToolTip.text: qsTr("Help for this window")
            Accessible.name: qsTr("Help for this window")
        }
    }

    footer: DialogButtonBox {
        visible: count > 0
    }

    T.Overlay.modal: Rectangle { color: Theme.withAlpha(Theme.showBg, 0.6) }
    T.Overlay.modeless: Rectangle { color: Theme.withAlpha(Theme.showBg, 0.12) }
}
