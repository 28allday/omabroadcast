import QtQuick
import QtWebEngine

// One browser source's page, drawn off screen by the helper. Transparent
// where the page is; no menus, dialogs or new windows (nobody can see them):
// what would open one is answered here, or Qt WebEngine's own opens where no one can.
WebEngineView {
    id: view
    // What the helper tells the app: {loading, error, title, crashed}.
    signal report(var state)
    property string storage: ""
    property string css: ""

    backgroundColor: "transparent"
    settings.playbackRequiresUserGesture: false
    settings.showScrollBars: false
    settings.focusOnNavigationEnabled: false
    // Each source keeps its own sign-ins and site data, in the folder the
    // app gives (a named profile is kept on disk).
    profile: WebEngineProfile {
        storageName: "omabroadcast-browser"
        persistentStoragePath: view.storage
        cachePath: view.storage === "" ? "" : view.storage + "/cache"
    }

    // The custom CSS goes in after the page's own, again on every load.
    function cssCode(text) {
        return "(function(){var id='omabroadcast-custom-css';var s=document.getElementById(id);"
             + "if(!s){s=document.createElement('style');s.id=id;(document.head||document.documentElement).appendChild(s);}"
             + "s.textContent=" + JSON.stringify(text) + ";})();"
    }
    function applyCss(text) {
        css = text
        userScripts.clear()
        const script = WebEngine.script()
        script.name = "omabroadcast-custom-css"
        script.sourceCode = cssCode(text)
        script.injectionPoint = WebEngineScript.DocumentReady
        script.worldId = WebEngineScript.ApplicationWorld
        script.runsOnSubFrames = false
        userScripts.insert(script)
        runJavaScript(cssCode(text), WebEngineScript.ApplicationWorld)
    }

    property string lastError: ""
    onLoadingChanged: (info) => {
        if (info.status === WebEngineView.LoadStartedStatus) lastError = ""
        // A refused sign-in also ends the load in failure: saying why is the more use.
        else if (info.status === WebEngineView.LoadFailedStatus && lastError === "") lastError = info.errorString !== "" ? info.errorString : "could not load"
        report({ loading: loading, error: lastError, title: title, crashed: false })
    }
    onTitleChanged: report({ loading: loading, error: lastError, title: title, crashed: false })
    // A page that crashes comes back by itself a moment later.
    onRenderProcessTerminated: (status, code) => {
        report({ loading: false, error: "", title: title, crashed: true })
        revive.restart()
    }
    Timer { id: revive; interval: 1000; onTriggered: view.reload() }
    onJavaScriptDialogRequested: (request) => { request.accepted = true; request.dialogAccept() }
    onContextMenuRequested: (request) => { request.accepted = true }
    onFullScreenRequested: (request) => request.reject()
    // These the page may wait on for ever: refused, and the source says why.
    function refuse(why) {
        lastError = why
        report({ loading: loading, error: lastError, title: title, crashed: false })
    }
    onAuthenticationDialogRequested: (request) => {
        request.accepted = true
        request.dialogReject()
        refuse("the page asks for a password, which a source cannot show")
    }
    onFileDialogRequested: (request) => {
        request.accepted = true
        request.dialogReject()
        refuse("the page asks for a file, which a source cannot show")
    }
    onColorDialogRequested: (request) => {
        request.accepted = true
        request.dialogReject()
        refuse("the page asks for a colour, which a source cannot show")
    }
    onSelectClientCertificate: (selection) => {
        selection.selectNone()
        refuse("the page asks for a certificate, which a source cannot show")
    }
}
