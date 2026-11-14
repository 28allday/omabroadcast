# The browser helper: built only where Qt WebEngine is installed. Without
# it the app still builds and runs, with browser sources and docks hidden.
qtHaveModule(webenginequick) {
    QT += core gui qml quick webenginequick
    CONFIG += c++20 release
    TEMPLATE = app
    TARGET = omabroadcast-browser
    INCLUDEPATH += ../src
    HEADERS += ../src/capture/browser_wire.h
    SOURCES += main.cpp
    RESOURCES += browser.qrc
} else {
    TEMPLATE = aux
    message("Qt WebEngine is not installed: building without browser sources.")
}
