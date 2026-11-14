pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Templates as T
import OmaBroadcast 1.0

// Dropdown: a filled well with a hairline and a chevron — the Fill / Border /
// Shadow selects in the concept inspector.
T.ComboBox {
    id: control
    // Any control shows its tooltip once it has words: users only set the text.
    T.ToolTip.visible: hovered && T.ToolTip.text !== "" && !popup.visible
    T.ToolTip.delay: Theme.tooltipDelay

    // A long choice never widens what holds it: it is cut short with "…" instead.
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            Math.min(implicitContentWidth, Theme.wInspector) + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    leftPadding: Theme.s2 + 2
    rightPadding: Theme.s2 + (indicator ? indicator.width + Theme.s2 : 0)
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsControl
    hoverEnabled: true
    opacity: enabled ? 1 : Theme.disabledOpacity

    // The open list is as wide as its longest line, so no choice is cut short.
    property real widestChoice: 0
    function measureChoices() {
        let widest = 0
        for (let i = 0; i < count; ++i) {
            choiceMetrics.text = textAt(i)
            widest = Math.max(widest, choiceMetrics.advanceWidth)
        }
        widestChoice = Math.ceil(widest)
    }
    TextMetrics { id: choiceMetrics; font: control.font }
    onCountChanged: if (popup.visible) measureChoices()

    delegate: T.ItemDelegate {
        id: option
        required property int index
        required property var model
        width: ListView.view ? ListView.view.width : control.width
        implicitHeight: Math.max(Theme.hRow, optionText.implicitHeight + Theme.s1 * 2)
        leftPadding: Theme.s2 + 2
        rightPadding: Theme.s2
        highlighted: control.highlightedIndex === index
        hoverEnabled: control.hoverEnabled
        text: model[control.textRole]
        font: control.font
        contentItem: Row {
            spacing: Theme.s2
            Icon {
                name: "check"
                size: Theme.szIcon - 2
                color: Theme.accent
                opacity: control.currentIndex === option.index ? 1 : 0
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                id: optionText
                text: option.text
                textFormat: Text.PlainText
                font: option.font
                color: option.highlighted ? Theme.textPrimary : Theme.textSecondary
                wrapMode: Text.Wrap
                width: option.availableWidth - Theme.szIcon - Theme.s2
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        background: Rectangle {
            radius: Theme.rControl
            color: option.highlighted ? Theme.controlHover : "transparent"
        }
    }

    indicator: Icon {
        name: "chevron-down"
        size: Theme.szIcon
        color: control.hovered ? Theme.textSecondary : Theme.textMuted
        x: control.width - width - Theme.s2
        y: control.topPadding + (control.availableHeight - height) / 2
    }

    // Typed into: a field. Chosen from: a label that ends in "…" when it does not fit.
    contentItem: control.editable ? editField : shownLabel
    readonly property Text shownLabel: Text {
        text: control.displayText
        font: control.font
        color: control.flat ? Theme.textSecondary : Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    readonly property T.TextField editField: T.TextField {
        leftPadding: 0
        rightPadding: 0
        text: control.editable ? control.editText : control.displayText
        enabled: control.editable
        autoScroll: control.editable
        readOnly: control.down
        inputMethodHints: control.inputMethodHints
        validator: control.validator
        selectByMouse: control.selectTextByMouse
        font: control.font
        color: control.flat ? Theme.textSecondary : Theme.textPrimary
        selectionColor: Theme.withAlpha(Theme.accent, 0.35)
        selectedTextColor: Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        implicitWidth: Theme.wInspector / 2
        implicitHeight: Theme.hControl
        radius: Theme.rControl
        color: control.down ? Theme.pressedOn(Theme.controlBg)
             : control.hovered ? Theme.controlHover
             : control.flat ? "transparent" : Theme.controlBg
        border.width: control.flat && !control.visualFocus ? 0 : Theme.hairline
        border.color: control.visualFocus || control.activeFocus && control.editable ? Theme.accent : Theme.border
    }

    popup: T.Popup {
        y: control.height + 2
        readonly property real room: T.Overlay.overlay ? T.Overlay.overlay.width - leftMargin - rightMargin : Infinity
        width: Math.min(room, Math.max(control.width, Theme.wInspector / 2,
                                       control.widestChoice + Theme.szIcon + Theme.s2 * 3 + 2 + padding * 2))
        onAboutToShow: control.measureChoices()
        height: Math.min(contentItem.implicitHeight + topPadding + bottomPadding,
                         control.Window.height - topMargin - bottomMargin)
        topMargin: Theme.s2
        bottomMargin: Theme.s2
        leftMargin: Theme.s2
        rightMargin: Theme.s2
        padding: Theme.s1

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            highlightMoveDuration: 0
            boundsBehavior: Flickable.StopAtBounds
            T.ScrollIndicator.vertical: ScrollIndicator {}
        }
        background: Rectangle {
            color: Theme.panelRaised
            radius: Theme.rMenu
            border.width: Theme.hairline
            border.color: Theme.borderStrong
        }
    }
}
