import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// A colour as #rrggbb or #aarrggbb, with a swatch. Commits when the text
// is a colour and editing ends.
RowLayout {
    id: root
    // Shows its tooltip once it has words, like the style's controls.
    HoverHandler { id: tipHover }
    ToolTip.visible: tipHover.hovered && ToolTip.text !== ""
    ToolTip.delay: Theme.tooltipDelay
    property string value: "#000000"
    signal committed(string value)
    spacing: Theme.s2

    Rectangle {
        implicitWidth: Theme.hControl
        implicitHeight: Theme.hControl
        radius: Theme.rControl
        color: root.value
        border.width: Theme.hairline
        border.color: Theme.borderStrong
    }
    TextField {
        // tooltip: the field's own, shown over all of it
        Layout.fillWidth: true
        text: root.value
        font.family: Theme.monoFamily
        validator: RegularExpressionValidator { regularExpression: /^#?([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/ }
        onEditingFinished: {
            const v = text.startsWith("#") ? text : "#" + text
            if (acceptableInput && v.toLowerCase() !== root.value.toLowerCase()) root.committed(v)
            text = Qt.binding(() => root.value)
        }
        // Editing does not finish on what is not a colour: left half typed it goes
        // back, unless the window or a menu only took the focus for a moment.
        onActiveFocusChanged: if (!activeFocus && !acceptableInput && focusReason !== Qt.ActiveWindowFocusReason && focusReason !== Qt.PopupFocusReason)
            text = Qt.binding(() => root.value)
    }
}
