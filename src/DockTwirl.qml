import QtQuick
import QtQuick.Controls
import OmaBroadcast 1.0

// The arrow at the start of a dock's header: folds the whole dock down to
// its header, or opens it again. Folding keeps the dock where it is;
// closing it is in the ⋮ menu.
ToolButton {
    id: twirl
    property bool folded: false
    property string title
    objectName: "dockTwirl"
    icon.name: folded ? "chevron-right" : "chevron-down"
    implicitWidth: Theme.hControl
    padding: 0
    ToolTip.text: folded ? qsTr("Open %1").arg(title) : qsTr("Fold %1 away").arg(title)
    Accessible.name: ToolTip.text
}
