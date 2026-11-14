import QtQuick
import QtQuick.Templates as T
import OmaBroadcast 1.0

T.CheckBox {
    id: control
    // Any control shows its tooltip once it has words: users only set the text.
    T.ToolTip.visible: hovered && T.ToolTip.text !== ""
    T.ToolTip.delay: Theme.tooltipDelay

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    padding: Theme.s1
    spacing: Theme.s2
    hoverEnabled: true
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsControl
    opacity: enabled ? 1 : Theme.disabledOpacity

    indicator: Rectangle {
        implicitWidth: Theme.szIcon
        implicitHeight: Theme.szIcon
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: Theme.rHandle + 1
        color: control.checkState !== Qt.Unchecked ? Theme.accent : Theme.controlBg
        border.width: Theme.hairline
        border.color: control.checkState !== Qt.Unchecked ? Theme.accent
                    : control.visualFocus || control.hovered ? Theme.textMuted : Theme.borderStrong
        Icon {
            anchors.centerIn: parent
            visible: control.checkState === Qt.Checked
            name: "check"
            size: parent.width - 2
            color: Theme.accentText
        }
        Rectangle {
            anchors.centerIn: parent
            visible: control.checkState === Qt.PartiallyChecked
            width: parent.width / 2; height: 2
            color: Theme.accentText
        }
    }

    contentItem: Text {
        leftPadding: control.indicator && !control.mirrored ? control.indicator.width + control.spacing : 0
        text: control.text
        textFormat: Text.PlainText
        font: control.font
        color: Theme.textPrimary
        elide: Text.ElideRight
        wrapMode: Text.Wrap
        verticalAlignment: Text.AlignVCenter
    }
}
