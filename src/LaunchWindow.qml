import QtQuick
import QtQuick.Controls
import QtQuick.Window
import OmaBroadcast 1.0

// The card at a normal start: see launch_screen.h. OK or Return goes on to the
// program; Escape, or closing it, leaves.
ApplicationWindow {
    id: root
    objectName: "launchWindow"
    property string version: ""
    property var quote: ({})
    property bool confirmed: false
    property bool playbackReady: false

    readonly property int cardWidth: 560
    readonly property int cardHeight: 380
    readonly property int margin: 32
    // Kept together on a small screen, as the window is a fixed size.
    readonly property real layoutScale: Math.max(0.25, Math.min(1,
        (Screen.desktopAvailableWidth - margin) / cardWidth,
        (Screen.desktopAvailableHeight - margin) / cardHeight))

    function enter() { confirmed = true; close() }

    visible: false
    title: qsTr("Welcome to OmaBroadcast")
    flags: Qt.Dialog | Qt.FramelessWindowHint
    width: Math.floor(cardWidth * layoutScale)
    height: Math.floor(cardHeight * layoutScale)
    minimumWidth: width
    maximumWidth: width
    minimumHeight: height
    maximumHeight: height
    color: Theme.panelBg

    // Still until the compositor has the first frame, so the animation does
    // not start while the window is still arriving.
    onFrameSwapped: if (!playbackDelay.running && !playbackReady) playbackDelay.start()
    Timer {
        id: playbackDelay
        interval: 750
        onTriggered: root.playbackReady = root.visible
    }

    Shortcut { sequence: "Return"; enabled: !buildDetails.opened; onActivated: root.enter() }
    Shortcut { sequence: "Enter"; enabled: !buildDetails.opened; onActivated: root.enter() }
    Shortcut { sequence: "Escape"; enabled: !buildDetails.opened; onActivated: root.close() }

    Item {
        id: card
        width: root.cardWidth
        height: root.cardHeight
        scale: root.layoutScale
        transformOrigin: Item.TopLeft

        // No title bar, so the top of the card moves it.
        MouseArea {
            width: parent.width
            height: 130
            onPressed: root.startSystemMove()
        }
        Image {
            objectName: "launchLogo"
            x: root.margin
            y: 38
            width: 44
            height: 44
            source: "qrc:/launch/logo.svg"
            sourceSize: Qt.size(88, 88)
            fillMode: Image.PreserveAspectFit
        }
        Text {
            objectName: "launchTitle"
            x: 88
            y: 34
            height: 52
            text: Theme.appName
            color: Theme.textPrimary
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsDisplay
            verticalAlignment: Text.AlignVCenter
        }
        Text {
            objectName: "launchVersion"
            x: 88
            y: 94
            text: qsTr("Version %1").arg(root.version)
            color: Theme.textSecondary
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
        }
        AnimatedImage {
            objectName: "launchSpectrum"
            x: 334
            y: 34
            width: 194
            height: 85
            source: "qrc:/launch/spectrum.webp"
            fillMode: Image.PreserveAspectFit
            // Frames are read as they are shown: there are few, but they are large.
            cache: false
            playing: root.playbackReady && root.visible && !Theme.reducedMotion
        }
        Text {
            objectName: "launchRenderingStatus"
            x: root.margin
            y: 130
            visible: GraphicsInfo.api === GraphicsInfo.Software
            text: qsTr("Interface: drawn on the processor")
            color: Theme.textSecondary
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
        }
        Column {
            objectName: "launchQuotation"
            x: root.margin
            y: 154
            width: parent.width - root.margin * 2
            spacing: 10
            Text {
                objectName: "launchQuote"
                width: parent.width
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                visible: text !== ""
                text: root.quote.text ? "“" + root.quote.text + "”" : ""
                color: Theme.textPrimary
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsQuote
                lineHeight: 1.25
            }
            // The words go to where they were found.
            AbstractButton {
                id: author
                objectName: "launchAttribution"
                visible: !!root.quote.author
                width: attribution.implicitWidth
                height: 22
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.name: qsTr("Quote by %1. Open its source.").arg(root.quote.author || "")
                ToolTip.visible: hovered
                ToolTip.delay: Theme.tooltipDelay
                ToolTip.text: qsTr("Where this quote comes from")
                onClicked: Qt.openUrlExternally(root.quote.source)
                contentItem: Text {
                    id: attribution
                    text: "— " + (root.quote.author || "")
                    color: author.hovered || author.visualFocus ? Theme.accent : Theme.textSecondary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsLabel
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
        Item {
            objectName: "launchFooter"
            x: root.margin
            y: 316
            width: parent.width - root.margin * 2
            height: 48
            Text {
                objectName: "launchCopyright"
                text: qsTr("© 2026 Gavin Nugent")
                color: Theme.textSecondary
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel
            }
            AbstractButton {
                id: detailsButton
                objectName: "launchBuildDetails"
                y: 24
                width: details.implicitWidth
                height: 24
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.name: qsTr("Build and licence details")
                onClicked: buildDetails.open()
                contentItem: Text {
                    id: details
                    text: qsTr("Build & licence") + "  +"
                    color: detailsButton.hovered || detailsButton.visualFocus ? Theme.textPrimary : Theme.textSecondary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsLabel
                    verticalAlignment: Text.AlignVCenter
                }
            }
            Button {
                objectName: "launchOkay"
                anchors.right: parent.right
                width: 144
                height: 44
                highlighted: true
                focus: true
                text: qsTr("OK")
                ToolTip.text: qsTr("Open OmaBroadcast")
                onClicked: root.enter()
            }
        }
    }
    Popup {
        id: buildDetails
        objectName: "launchBuildPopup"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(root.width - root.margin * root.layoutScale, 480 * root.layoutScale)
        padding: 20 * root.layoutScale
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        contentItem: Column {
            spacing: 14 * root.layoutScale
            Text {
                objectName: "launchFullVersion"
                width: parent.width
                wrapMode: Text.WrapAnywhere
                text: Theme.appName + " " + root.version
                color: Theme.textPrimary
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsSection * root.layoutScale
            }
            Text {
                objectName: "launchLicence"
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Copyright © 2026 Gavin Nugent.\n\nOmaBroadcast's own code is under the MIT licence. The package, with the FFmpeg it carries, is under the GNU GPL v3. Free software, provided without warranty.\n\nEvery licence: /usr/share/licenses/omabroadcast/")
                color: Theme.textSecondary
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel * root.layoutScale
                lineHeight: 1.25
            }
            Button {
                objectName: "launchCloseDetails"
                text: qsTr("Close")
                ToolTip.text: qsTr("Back to the launch window")
                onClicked: buildDetails.close()
            }
        }
    }
}
