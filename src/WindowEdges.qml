import QtQuick
import OmaBroadcast 1.0

// The edges of a window with no frame of its own: eight places to take hold and resize it,
// and a mark in the corner for those who do not know an edge will do. Put it last in the
// window, so it lies above everything; it takes the pointer at the edges only.
Item {
    id: edges

    // The window to resize.
    required property var window
    // Begins each object name: "<prefix>Resize-left" and so on.
    required property string prefix

    anchors.fill: parent

    Repeater {
        model: [
            { name: "left", edges: Qt.LeftEdge, cursor: Qt.SizeHorCursor },
            { name: "right", edges: Qt.RightEdge, cursor: Qt.SizeHorCursor },
            { name: "top", edges: Qt.TopEdge, cursor: Qt.SizeVerCursor },
            { name: "bottom", edges: Qt.BottomEdge, cursor: Qt.SizeVerCursor },
            { name: "topLeft", edges: Qt.TopEdge | Qt.LeftEdge, cursor: Qt.SizeFDiagCursor },
            { name: "bottomRight", edges: Qt.BottomEdge | Qt.RightEdge, cursor: Qt.SizeFDiagCursor },
            { name: "topRight", edges: Qt.TopEdge | Qt.RightEdge, cursor: Qt.SizeBDiagCursor },
            { name: "bottomLeft", edges: Qt.BottomEdge | Qt.LeftEdge, cursor: Qt.SizeBDiagCursor }
        ]
        MouseArea {
            required property var modelData
            readonly property bool atLeft: modelData.edges & Qt.LeftEdge
            readonly property bool atRight: modelData.edges & Qt.RightEdge
            readonly property bool atTop: modelData.edges & Qt.TopEdge
            readonly property bool atBottom: modelData.edges & Qt.BottomEdge
            readonly property int grab: atLeft || atRight ? (atTop || atBottom ? Theme.s4 : Theme.s2) : Theme.s2
            objectName: edges.prefix + "Resize-" + modelData.name
            x: atRight ? edges.width - width : (atLeft ? 0 : Theme.s4)
            y: atBottom ? edges.height - height : (atTop ? 0 : Theme.s4)
            width: atLeft || atRight ? grab : edges.width - Theme.s4 * 2
            height: atTop || atBottom ? (atLeft || atRight ? Theme.s4 : Theme.s2) : edges.height - Theme.s4 * 2
            // Corners over edges.
            z: (atLeft || atRight) && (atTop || atBottom) ? 2 : 1
            cursorShape: modelData.cursor
            onPressed: edges.window.startSystemResize(modelData.edges)
        }
    }
    Text {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 1
        text: "◢"
        color: Theme.textMuted
        font.pixelSize: Theme.fsLabel
    }
}
