PKGCONFIG += wayland-client gbm libdrm
PROTOCOL_DIR = $$OUT_PWD/direct-protocols
!mkpath($$PROTOCOL_DIR): error(Cannot create Wayland protocol build directory)
INCLUDEPATH += $$PROTOCOL_DIR
PROTOCOL_NAMES = linux-dmabuf-v1 hyprland-toplevel-export-v1 wlr-screencopy-unstable-v1 wlr-foreign-toplevel-management-unstable-v1
for(protocol, PROTOCOL_NAMES) {
    xml = $$PWD/src/protocols/$${protocol}.xml
    client = $$PROTOCOL_DIR/$${protocol}-client.h
    server = $$PROTOCOL_DIR/$${protocol}-server.h
    code = $$PROTOCOL_DIR/$${protocol}.c
    # Made again only when missing or older than their XML: a fresh header
    # every qmake run would rebuild everything that includes it.
    !system(test $$shell_quote($$client) -nt $$shell_quote($$xml) || wayland-scanner client-header $$shell_quote($$xml) $$shell_quote($$client)): error(Wayland client generation failed)
    !system(test $$shell_quote($$server) -nt $$shell_quote($$xml) || wayland-scanner server-header $$shell_quote($$xml) $$shell_quote($$server)): error(Wayland server generation failed)
    !system(test $$shell_quote($$code) -nt $$shell_quote($$xml) || wayland-scanner private-code $$shell_quote($$xml) $$shell_quote($$code)): error(Wayland protocol generation failed)
    SOURCES += $$code
}
