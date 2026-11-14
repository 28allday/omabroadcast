#include "private_sink.h"

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <atomic>
#include <mutex>

namespace {
std::once_flag gPwInit;
}

struct PrivateSink::Impl {
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_proxy *proxy = nullptr;
    spa_hook proxyListener{}, coreListener{};
    std::function<void(bool)> ready;
    std::atomic<bool> told{false};

    void tell(bool ok) {
        if (!told.exchange(true) && ready) ready(ok);
    }
    static void bound(void *data, uint32_t) { static_cast<Impl *>(data)->tell(true); }
    static void proxyError(void *data, int, int, const char *) { static_cast<Impl *>(data)->tell(false); }
    static void coreError(void *data, uint32_t, int, int, const char *) { static_cast<Impl *>(data)->tell(false); }
};

PrivateSink::PrivateSink(const std::string &nodeName, const std::string &description, int channels,
                         std::function<void(bool)> ready)
    : m_name(nodeName), d(new Impl) {
    d->ready = std::move(ready);
    std::call_once(gPwInit, [] { pw_init(nullptr, nullptr); });
    d->loop = pw_thread_loop_new("omabroadcast-sink", nullptr);
    d->context = d->loop ? pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0) : nullptr;
    if (!d->context || pw_thread_loop_start(d->loop) < 0) {
        d->tell(false);
        return;
    }
    pw_thread_loop_lock(d->loop);
    d->core = pw_context_connect(d->context, nullptr, 0);
    if (d->core) {
        static const pw_core_events coreEvents = [] {
            pw_core_events e{};
            e.version = PW_VERSION_CORE_EVENTS;
            e.error = Impl::coreError;
            return e;
        }();
        pw_core_add_listener(d->core, &d->coreListener, &coreEvents, d);
        const std::string position = channels == 1 ? "MONO" : channels == 6 ? "FL,FR,FC,LFE,RL,RR" : "FL,FR";
        pw_properties *props = pw_properties_new(
            "factory.name", "support.null-audio-sink", PW_KEY_MEDIA_CLASS, "Audio/Sink",
            PW_KEY_NODE_NAME, nodeName.c_str(), PW_KEY_NODE_DESCRIPTION, description.c_str(),
            "audio.position", position.c_str(), PW_KEY_NODE_VIRTUAL, "true",
            // Never the default output, nor remembered as anybody's choice.
            PW_KEY_PRIORITY_SESSION, "0", PW_KEY_PRIORITY_DRIVER, "0", "object.linger", "false",
            "state.restore-props", "false", "monitor.channel-volumes", "true",
            PW_KEY_APP_ID, "org.omarchy.OmaBroadcast", nullptr);
        d->proxy = static_cast<pw_proxy *>(pw_core_create_object(d->core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
                                                                 &props->dict, 0));
        pw_properties_free(props);
        if (d->proxy) {
            static const pw_proxy_events proxyEvents = [] {
                pw_proxy_events e{};
                e.version = PW_VERSION_PROXY_EVENTS;
                e.bound = Impl::bound;
                e.error = Impl::proxyError;
                return e;
            }();
            pw_proxy_add_listener(d->proxy, &d->proxyListener, &proxyEvents, d);
        }
    }
    const bool started = d->core && d->proxy;
    pw_thread_loop_unlock(d->loop);
    if (!started) d->tell(false);
}

PrivateSink::~PrivateSink() {
    if (d->loop) {
        pw_thread_loop_lock(d->loop);
        if (d->proxy) {
            spa_hook_remove(&d->proxyListener);
            pw_proxy_destroy(d->proxy);
        }
        if (d->core) {
            spa_hook_remove(&d->coreListener);
            pw_core_disconnect(d->core);
        }
        pw_thread_loop_unlock(d->loop);
        pw_thread_loop_stop(d->loop);
    }
    if (d->context) pw_context_destroy(d->context);
    if (d->loop) pw_thread_loop_destroy(d->loop);
    delete d;
}

bool PrivateSink::pulseAvailable() {
    // PulseAudio's own server or PipeWire's stand-in for it, where clients look.
    QString server = qEnvironmentVariable("PULSE_SERVER");
    if (server.startsWith(QStringLiteral("unix:"))) server = server.mid(5);
    if (!server.isEmpty()) return !server.startsWith(QLatin1Char('/')) || QFileInfo::exists(server);
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return !runtime.isEmpty() && QFileInfo::exists(runtime + QStringLiteral("/pulse/native"));
}
