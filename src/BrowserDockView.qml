import QtQuick
import QtWebEngine

// A custom browser dock's page: an ordinary web view in the window. Loaded
// only where Qt WebEngine is installed (backend.webDocksAvailable), so the
// app runs without it. Every dock shares one profile: sign in once.
WebEngineView {
    id: page
    objectName: "browserDockPage"
    required property var sharedProfile
    required property string address
    profile: sharedProfile
    url: address
    settings.focusOnNavigationEnabled: false
    // Links someone clicks that want a window of their own open in the desktop's
    // browser. Nothing else leaves: a page must not be able to start another
    // program by its scheme, or open windows over a show by itself.
    onNewWindowRequested: (request) => {
        const scheme = request.requestedUrl.toString().split(":")[0].toLowerCase()
        if (request.userInitiated && (scheme === "http" || scheme === "https")) Qt.openUrlExternally(request.requestedUrl)
    }
}
