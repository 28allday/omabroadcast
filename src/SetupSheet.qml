import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaBroadcast 1.0

// Tools → Set up for…, and on a fresh install: what it is for, the pieces
// to use, a check of this machine (encoder speeds, and for a stream the
// upload), then what will be set and why. Creating makes a new profile and
// a new studio collection; nothing already there changes.
Sheet {
    id: sheet
    helpTopic: "setup"
    objectName: "setupSheet"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 640)
    height: Math.min(parent.height - Theme.s5 * 2, implicitHeight)
    modal: true
    closePolicy: backend.setupBusy ? Popup.NoAutoClose : Popup.CloseOnEscape
    title: [qsTr("What are you setting up for?"), qsTr("What to use"), qsTr("Checking this machine"), qsTr("Here is the set-up")][page]

    property int page: 0
    property string scenario: "record"
    property var displays: []
    readonly property bool streaming: scenario === "stream"
    readonly property var service: streaming ? backend.streamServices[serviceBox.currentIndex] : null
    readonly property var uploadTest: streaming && service ? backend.setupUploadTest(service.id) : ({ canTest: false })

    function begin() {
        page = 0
        displays = backend.setupDisplays()
        micBox.chosen = "none"
        micBox.showChosen()
        desktopBox.checked = false
        open()
    }
    function choices() {
        const camera = cameraBox.currentIndex > 0 ? backend.videoDevices[cameraBox.currentIndex - 1] : null
        return {
            scenario: scenario,
            display: displayBox.currentIndex,
            screen: screenBox.checked,
            camera: camera ? camera.name : "",
            cameraName: camera ? camera.description : "",
            microphone: micBox.currentValue === undefined ? "none" : micBox.currentValue,
            microphoneName: micBox.currentValue ? micBox.currentText : "",
            desktopSound: desktopBox.checked,
            service: service ? service.id : "",
            server: serverBox.currentValue === undefined ? "" : serverBox.currentValue,
            key: keyField.text,
            fastMotion: gamesButton.checked,
            uploadKbps: uploadTest.canTest ? 0 : Math.round(uploadField.value * 1000)
        }
    }

    Component.onCompleted: if (backend.setupOffered) begin()
    onClosed: if (backend.setupOffered) backend.dismissSetupOffer()
    Connections {
        target: backend
        function onSetupChanged() {
            if (sheet.page === 2 && !backend.setupBusy) sheet.page = backend.setupResult.cancelled ? 1 : 3
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.s3

        StackLayout {
            Layout.fillWidth: true
            currentIndex: sheet.page

            // 0: what for
            ColumnLayout {
                spacing: Theme.s1
                Label {
                    Layout.fillWidth: true
                    Layout.bottomMargin: Theme.s2
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                    text: qsTr("Pick one and OmaBroadcast tests what this machine can do, then sets up a profile and starter studios to match. You can change anything afterwards.")
                }
                Repeater {
                    model: backend.setupScenarios
                    delegate: ItemDelegate {
                        required property var modelData
                        objectName: "setupScenario_" + modelData.id
                        Layout.fillWidth: true
                        highlighted: sheet.scenario === modelData.id
                        onClicked: sheet.scenario = modelData.id
                        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: modelData.blurb
                        Accessible.name: modelData.name
                        contentItem: ColumnLayout {
                            spacing: 2
                            Label { text: modelData.name; font.bold: true }
                            Label { Layout.fillWidth: true; text: modelData.blurb; wrapMode: Text.Wrap; color: Theme.textSecondary }
                        }
                    }
                }
            }

            // 1: the pieces
            GridLayout {
                columns: 2
                columnSpacing: Theme.s3
                rowSpacing: Theme.s2

                Label { text: qsTr("Display") }
                ComboBox {
                    id: displayBox
                    objectName: "setupDisplay"
                    Layout.fillWidth: true
                    model: sheet.displays
                    textRole: "label"
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: qsTr("The canvas takes this display's size")
                    Accessible.name: qsTr("Display")
                }

                Label { text: qsTr("Screen") }
                CheckBox {
                    id: screenBox
                    objectName: "setupScreen"
                    text: qsTr("Show this display in the studios")
                    // Asked for only where the choice itself is about the screen.
                    checked: sheet.scenario === "tutorial" || sheet.scenario === "replay"
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: qsTr("Adds a capture of the display to the starter studios. You choose it again in the desktop's picker when the studios open.")
                    Accessible.name: qsTr("Screen")
                }

                Label { text: qsTr("Camera") }
                ComboBox {
                    id: cameraBox
                    objectName: "setupCamera"
                    Layout.fillWidth: true
                    model: [qsTr("None")].concat(backend.videoDevices.map(d => d.description))
                    currentIndex: sheet.scenario === "camera" && backend.videoDevices.length > 0 ? 1 : 0
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: qsTr("A webcam or capture card to put in the studios")
                    Accessible.name: qsTr("Camera")
                }

                Label { text: qsTr("Microphone") }
                ComboBox {
                    id: micBox
                    objectName: "setupMicrophone"
                    Layout.fillWidth: true
                    // Off until one is chosen; the choice stays when the list of inputs changes.
                    property string chosen: "none"
                    model: backend.microphones
                    textRole: "label"
                    valueRole: "value"
                    // One that has gone from the list (unplugged) is Off again, never
                    // Automatic, which would bring in an input nobody picked.
                    function showChosen() {
                        if (indexOfValue(chosen) < 0) chosen = "none"
                        currentIndex = indexOfValue(chosen)
                    }
                    onModelChanged: showChosen()
                    Component.onCompleted: showChosen()
                    onActivated: chosen = currentValue
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: qsTr("Your voice, off until you choose one. A microphone you pick is added to Sources with the camera; Automatic follows the desktop's default input and shows in the mixer only.")
                    Accessible.name: qsTr("Microphone")
                }

                Label { text: qsTr("Desktop sound") }
                CheckBox {
                    id: desktopBox
                    objectName: "setupDesktopSound"
                    text: qsTr("Record the computer's sound")
                    checked: false
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: qsTr("Everything the computer plays, notifications too")
                    Accessible.name: qsTr("Desktop sound")
                }

                Label { text: qsTr("Service"); visible: sheet.streaming }
                ComboBox {
                    id: serviceBox
                    objectName: "setupService"
                    visible: sheet.streaming
                    Layout.fillWidth: true
                    model: backend.streamServices
                    textRole: "name"
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: qsTr("Where the stream goes")
                    Accessible.name: qsTr("Service")
                }

                Label { text: qsTr("Server"); visible: sheet.streaming }
                ComboBox {
                    id: serverBox
                    objectName: "setupServer"
                    visible: sheet.streaming
                    Layout.fillWidth: true
                    model: sheet.service ? sheet.service.servers : []
                    textRole: "name"
                    valueRole: "url"
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: qsTr("The nearest one is usually best")
                    Accessible.name: qsTr("Server")
                }

                Label { text: sheet.service ? sheet.service.keyLabel : qsTr("Stream key"); visible: sheet.streaming }
                TextField {
                    id: keyField
                    objectName: "setupKey"
                    visible: sheet.streaming
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    placeholderText: qsTr("From the service's dashboard")
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: qsTr("Kept in the desktop's keyring. Needed for the upload test; can be added later otherwise.")
                    Accessible.name: qsTr("Stream key")
                }

                Label { text: qsTr("On screen"); visible: sheet.streaming }
                RowLayout {
                    visible: sheet.streaming
                    spacing: Theme.s3
                    RadioButton {
                        id: gamesButton
                        objectName: "setupGames"
                        checked: true
                        text: qsTr("Games and fast motion")
                        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("60 frames a second where the connection carries it")
                    }
                    RadioButton {
                        objectName: "setupTalking"
                        text: qsTr("Talking and desktop")
                        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("30 frames a second, leaving the bits for detail")
                    }
                }

                Label { text: qsTr("Upload speed"); visible: sheet.streaming && !sheet.uploadTest.canTest }
                RowLayout {
                    visible: sheet.streaming && !sheet.uploadTest.canTest
                    spacing: Theme.s2
                    SpinBox {
                        id: uploadField
                        objectName: "setupUpload"
                        from: 0; to: 1000; value: 0; editable: true
                        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                        ToolTip.text: qsTr("Megabits a second, from any speed test site; 0 if you don't know")
                        Accessible.name: qsTr("Upload speed in megabits a second")
                    }
                    Label { text: qsTr("Mb/s"); color: Theme.textMuted }
                }
                Label {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    visible: sheet.streaming
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                    text: sheet.uploadTest.canTest
                          ? qsTr("Your upload is measured with a short test stream that never goes live.")
                          : sheet.uploadTest.why || ""
                }
            }

            // 2: checking
            ColumnLayout {
                spacing: Theme.s3
                Label {
                    objectName: "setupStep"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: backend.setupStep
                }
                ProgressBar {
                    Layout.fillWidth: true
                    value: backend.setupProgress
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                    text: qsTr("Each encoder is tried at the sizes that could suit; this takes up to a minute.")
                }
            }

            // 3: the set-up
            ColumnLayout {
                spacing: Theme.s2
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s2
                    visible: backend.setupResult.ok === true
                    // In the reasons' column below.
                    Label { text: qsTr("Profile name"); color: Theme.textSecondary; Layout.preferredWidth: Theme.s5 * 5; elide: Text.ElideRight }
                    TextField {
                        id: nameField
                        objectName: "setupName"
                        Layout.fillWidth: true
                        text: backend.setupResult.name || ""
                        ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                        ToolTip.text: qsTr("The new profile and studio collection take this name")
                        Accessible.name: qsTr("Profile name")
                    }
                }
                Repeater {
                    model: backend.setupResult.reasons || []
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 0
                        RowLayout {
                            spacing: Theme.s2
                            Label { text: modelData.what; color: Theme.textSecondary; Layout.preferredWidth: Theme.s5 * 5 }
                            Label { text: modelData.value; font.bold: true; Layout.fillWidth: true; wrapMode: Text.Wrap }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: Theme.s5 * 5 + Theme.s2
                            text: modelData.why
                            wrapMode: Text.Wrap
                            color: Theme.textMuted
                        }
                    }
                }
                Repeater {
                    model: backend.setupResult.warnings || []
                    delegate: Label {
                        required property string modelData
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.warning
                        text: modelData
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.s2
            spacing: Theme.s2
            Button {
                objectName: "setupSkip"
                visible: sheet.page === 0
                text: backend.setupOffered ? qsTr("Set up myself") : qsTr("Close")
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                ToolTip.text: qsTr("Leave the settings as they are; Tools → Set up for… brings this back")
                onClicked: sheet.close()
            }
            Button {
                objectName: "setupBack"
                visible: sheet.page === 1 || sheet.page === 3
                text: qsTr("Back")
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("The step before")
                onClicked: sheet.page = sheet.page === 3 ? 1 : 0
            }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "setupCancel"
                visible: sheet.page === 2
                text: qsTr("Cancel")
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Stop checking")
                onClicked: backend.cancelSetupCheck()
            }
            Button {
                objectName: "setupNext"
                visible: sheet.page < 2
                highlighted: true
                text: sheet.page === 0 ? qsTr("Next") : qsTr("Check this machine")
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                ToolTip.text: sheet.page === 0 ? qsTr("Choose what to use") : qsTr("Measure the encoders (and the upload) and work out the set-up")
                onClicked: {
                    if (sheet.page === 0) { sheet.page = 1; return }
                    sheet.page = 2
                    backend.startSetupCheck(sheet.choices())
                }
            }
            Button {
                objectName: "setupCreate"
                visible: sheet.page === 3
                enabled: backend.setupResult.ok === true && nameField.text.trim().length > 0
                highlighted: true
                text: qsTr("Create")
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                ToolTip.text: qsTr("Make the profile and studios and switch to them")
                onClicked: if (backend.applySetup(nameField.text)) sheet.close()
            }
        }
    }
}
