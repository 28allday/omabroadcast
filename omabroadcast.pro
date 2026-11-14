QT += core gui qml quick quickcontrols2 dbus network

CONFIG += c++20 release link_pkgconfig
PKGCONFIG += rnnoise vulkan libpipewire-0.3 libavcodec libavformat libavfilter libavutil libswresample libswscale
TARGET = omabroadcast
TEMPLATE = app

# The FFmpeg 9.1 it carries (bin/build-ffmpeg): beside the build while
# developing, then where the package installs it.
carried_ffmpeg {
    # Its headers ahead of /usr/include, which another package's flags name.
    INCLUDEPATH = $$CARRIED_FFMPEG/include $$INCLUDEPATH
    QMAKE_LFLAGS += -Wl,--enable-new-dtags
    # The copy beside the build only while developing: the package's binary
    # looks in its install place alone.
    !packaged: QMAKE_LFLAGS += "-Wl,-rpath,\'\$$ORIGIN/ffmpeg/usr/lib/omabroadcast/ffmpeg/lib\'"
    QMAKE_LFLAGS += -Wl,-rpath,/usr/lib/omabroadcast/ffmpeg/lib
}

INCLUDEPATH += src

# The version the program shows, from the package's own numbers. The header is
# written only when they change, so that main.cpp is compiled again only then.
APP_VERSION = $$system("sed -n 's/^pkgver=//p' $$PWD/pkgbuild/PKGBUILD")-$$system("sed -n 's/^pkgrel=//p' $$PWD/pkgbuild/PKGBUILD")
!packaged: APP_VERSION = $${APP_VERSION}-dev
APP_VERSION_LINE = "$${LITERAL_HASH}define OMABROADCAST_VERSION \"$${APP_VERSION}\""
!equals(APP_VERSION_LINE, $$cat($$OUT_PWD/app_version.h, blob)): write_file($$OUT_PWD/app_version.h, APP_VERSION_LINE)
DEPENDPATH += $$OUT_PWD

HEADERS += \
    src/backend.h \
    src/launch_screen.h \
    src/core/prompter_script.h \
    src/core/recording_player.h \
    src/filepicker.h \
    src/theme.h \
    src/preview_view.h \
    src/scope_view.h \
    src/engine/frame.h \
    src/engine/engine.h \
    src/engine/matting.h \
    src/engine/row_pool.h \
    src/capture/screencast_portal.h \
    src/capture/video_stream.h \
    src/capture/video_devices.h \
    src/capture/device_stream.h \
    src/capture/device_controls.h \
    src/capture/picture.h \
    src/capture/media_stream.h \
    src/audio/noise_suppression.h \
    src/audio/filters.h \
    src/audio/tempo.h \
    src/core/audio_filter_settings.h \
    src/core/video_filter_settings.h \
    src/audio/peak_meter.h \
    src/core/meter_settings.h \
    src/audio/format.h \
    src/audio/pipewire_format.h \
    src/audio/push_control.h \
    src/control/global_shortcuts.h \
    src/control/hyprland_rules.h \
    src/control/region_drawer.h \
    src/control/control_socket.h \
    src/control/omarchy_bar.h \
    src/control/remote_control.h \
    src/control/websocket.h \
    src/control/focus_watcher.h \
    src/control/scripts.h \
    src/control/secrets.h \
    src/control/hotkeys.h \
    src/audio/mixer.h \
    src/audio/audio_playback.h \
    src/audio/audio_capture.h \
    src/audio/audio_devices.h \
    src/output/encoders.h \
    src/output/encoder_helper.h \
    src/output/encoder_bench.h \
    src/output/recorder.h \
    src/output/encoded_stream.h \
    src/output/queued_sink.h \
    src/output/file_sink.h \
    src/output/remux.h \
    src/output/replay_sink.h \
    src/output/virtual_camera.h \
    src/output/encodings.h \
    src/output/stream_sink.h \
    src/output/services.h \
    src/output/bandwidth_test.h \
    src/core/audio_settings.h \
    src/core/profile.h \
    src/core/presets.h \
    src/core/machine_survey.h \
    src/core/starter_studios.h \
    src/core/scene.h \
    src/core/region.h \
    src/core/hardware.h \
    src/core/deinterlace.h \
    src/core/production.h \
    src/core/transitions.h \
    src/core/drawn.h \
    src/core/session.h

SOURCES += \
    src/main.cpp \
    src/launch_screen.cpp \
    src/backendprompter.cpp \
    src/backendplayback.cpp \
    src/core/recording_player.cpp \
    src/core/prompter_script.cpp \
    src/backend.cpp \
    src/backendcollections.cpp \
    src/backendfilters.cpp \
    src/backendoutputs.cpp \
    src/backendprofiles.cpp \
    src/backendscenes.cpp \
    src/backendsetup.cpp \
    src/backendtransitions.cpp \
    src/backendviews.cpp \
    src/backendactions.cpp \
    src/backendtools.cpp \
    src/backendremote.cpp \
    src/control/focus_watcher.cpp \
    src/control/scripts.cpp \
    src/control/control_socket.cpp \
    src/control/omarchy_bar.cpp \
    src/control/remote_control.cpp \
    src/control/websocket.cpp \
    src/filepicker.cpp \
    src/theme.cpp \
    src/preview_view.cpp \
    src/scope_view.cpp \
    src/engine/cpu_engine.cpp \
    src/engine/matting.cpp \
    src/engine/vk_engine.cpp \
    src/capture/screencast_portal.cpp \
    src/capture/video_stream.cpp \
    src/capture/video_devices.cpp \
    src/capture/device_stream.cpp \
    src/capture/device_controls.cpp \
    src/capture/media_stream.cpp \
    src/audio/noise_suppression.cpp \
    src/audio/filters.cpp \
    src/audio/tempo.cpp \
    src/core/audio_filter_settings.cpp \
    src/core/video_filter_settings.cpp \
    src/audio/peak_meter.cpp \
    src/control/global_shortcuts.cpp \
    src/control/hyprland_rules.cpp \
    src/control/region_drawer.cpp \
    src/control/secrets.cpp \
    src/control/hotkeys.cpp \
    src/audio/mixer.cpp \
    src/audio/audio_playback.cpp \
    src/audio/audio_capture.cpp \
    src/audio/audio_devices.cpp \
    src/output/encoders.cpp \
    src/output/encoder_helper.cpp \
    src/output/encoder_bench.cpp \
    src/output/recorder.cpp \
    src/output/encoded_stream.cpp \
    src/output/file_sink.cpp \
    src/output/remux.cpp \
    src/output/replay_sink.cpp \
    src/output/virtual_camera.cpp \
    src/output/encodings.cpp \
    src/output/stream_sink.cpp \
    src/output/services.cpp \
    src/output/bandwidth_test.cpp \
    src/core/profile.cpp \
    src/core/presets.cpp \
    src/core/machine_survey.cpp \
    src/core/starter_studios.cpp \
    src/core/scene.cpp \
    src/core/production.cpp \
    src/core/transitions.cpp \
    src/core/drawn.cpp \
    src/core/session.cpp

RESOURCES += src/resources.qrc src/engine/shaders/shaders.qrc
# The person-finding network's weights are 13 MB: resources go in as objects, not C++ arrays.
RESOURCES += src/engine/models/models.qrc
CONFIG += resources_big

# Translations: a translations/omabroadcast_<language>.ts listed in
# TRANSLATIONS is compiled and built in. Refresh the template and every
# translation with: lupdate -no-obsolete omabroadcast.pro -ts translations/*.ts
TRANSLATIONS =
# Only with a translation: rcc refuses an empty resource.
!isEmpty(TRANSLATIONS): CONFIG += lrelease embed_translations
QM_FILES_RESOURCE_PREFIX = /i18n

include(wayland.pri)
HEADERS += src/capture/direct_capture.h
SOURCES += src/capture/direct_capture.cpp

# Tools: the stats dock, the log and crash reports, safe mode, repair.
HEADERS += src/core/system_stats.h src/core/app_log.h src/core/crash_guard.h src/core/start_options.h src/core/repair.h
SOURCES += src/backendsystem.cpp src/core/session_tools.cpp src/core/system_stats.cpp src/core/app_log.cpp src/core/crash_guard.cpp src/core/repair.cpp

# Browser sources: the page is drawn by a helper program (browser/, built
# only where Qt WebEngine is installed); the app itself never links it.
HEADERS += src/capture/browser_page.h src/capture/browser_wire.h src/audio/private_sink.h
SOURCES += src/capture/browser_page.cpp src/audio/private_sink.cpp src/backendbrowser.cpp
