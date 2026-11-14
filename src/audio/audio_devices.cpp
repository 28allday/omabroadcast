#include "audio_devices.h"

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace {

std::string prop(const std::map<std::string, std::string> &props, const char *key) {
    const auto it = props.find(key);
    return it == props.end() ? std::string() : it->second;
}

bool isTrue(const std::string &v) { return v == "true" || v == "1"; }

// The session manager stores the default as JSON: {"name":"alsa_input..."}.
std::string nameFromJson(const char *json) {
    if (!json) return {};
    const char *key = std::strstr(json, "\"name\"");
    if (!key) return {};
    const char *colon = std::strchr(key + 6, ':');
    if (!colon) return {};
    const char *open = std::strchr(colon, '"');
    if (!open) return {};
    const char *close = std::strchr(open + 1, '"');
    return close ? std::string(open + 1, close) : std::string();
}

}  // namespace

bool describeAudioInput(uint32_t id, const std::map<std::string, std::string> &props, AudioInput *out) {
    const std::string cls = prop(props, PW_KEY_MEDIA_CLASS);
    if (cls != "Audio/Source" && cls != "Audio/Source/Virtual" && cls != "Audio/Duplex") return false;
    AudioInput in;
    in.id = id;
    in.name = prop(props, PW_KEY_NODE_NAME);
    if (in.name.empty()) return false;
    in.description = prop(props, PW_KEY_NODE_DESCRIPTION);
    if (in.description.empty()) in.description = prop(props, PW_KEY_NODE_NICK);
    if (in.description.empty()) in.description = in.name;
    try {
        in.priority = std::stoi(prop(props, PW_KEY_PRIORITY_SESSION));
    } catch (...) {
        in.priority = 0;
    }
    // Hardware: a device stands behind it, and it is not one of the kinds that
    // only look like hardware — a virtual node, or the kernel's loopback card,
    // which is an ALSA device that only ever hears what software plays into it.
    in.hardware = cls != "Audio/Source/Virtual" && !prop(props, PW_KEY_DEVICE_ID).empty() &&
                  !isTrue(prop(props, PW_KEY_NODE_VIRTUAL)) && prop(props, "alsa.driver_name") != "snd_aloop";
    if (out) *out = in;
    return true;
}

bool describeAudioNode(uint32_t id, const std::map<std::string, std::string> &props, AudioNode *out) {
    AudioNode node;
    const std::string cls = prop(props, PW_KEY_MEDIA_CLASS);
    if (!describeAudioInput(id, props, &node)) {
        if (cls == "Audio/Sink") node.kind = AudioNode::Output;
        else if (cls == "Stream/Output/Audio") node.kind = AudioNode::Application;
        else return false;
        node.id = id;
        node.name = prop(props, PW_KEY_NODE_NAME);
        if (node.name.empty()) return false;
        node.description = prop(props, PW_KEY_NODE_DESCRIPTION);
        if (node.description.empty()) node.description = node.name;
    }
    // Never offer our own monitoring/output nodes as capture targets.
    if (prop(props, PW_KEY_APP_ID) == "org.omarchy.OmaBroadcast") return false;
    node.serial = prop(props, PW_KEY_OBJECT_SERIAL);
    if (node.serial.empty()) node.serial = std::to_string(id);
    node.applicationName = prop(props, PW_KEY_APP_NAME);
    if (node.applicationName.empty()) node.applicationName = node.description;
    for (const char *key : {PW_KEY_APP_ID, PW_KEY_APP_PROCESS_BINARY, PW_KEY_APP_NAME, PW_KEY_NODE_NAME}) {
        const std::string value = prop(props, key);
        if (!value.empty()) { node.application = std::string(key) + ":" + value; break; }
    }
    if (out) *out = std::move(node);
    return true;
}

MicrophonePick pickMicrophone(const std::vector<AudioInput> &inputs, const std::string &choice,
                              const std::string &defaultName) {
    MicrophonePick pick;
    if (choice == "none") {
        pick.state = MicrophonePick::Off;
        return pick;
    }
    auto use = [&](const AudioInput &in) {
        pick.state = MicrophonePick::Using;
        pick.name = in.name;
        pick.description = in.description;
        return pick;
    };
    if (!choice.empty()) {
        for (const AudioInput &in : inputs)
            if (in.name == choice) return use(in);
        pick.state = MicrophonePick::Missing;
        pick.description = choice;
        return pick;
    }
    // Automatic: the desktop's default when it is real hardware, otherwise the
    // hardware input the session manager ranks highest.
    for (const AudioInput &in : inputs)
        if (in.hardware && in.name == defaultName) return use(in);
    const AudioInput *best = nullptr;
    for (const AudioInput &in : inputs) {
        if (!in.hardware) continue;
        if (!best || in.priority > best->priority || (in.priority == best->priority && in.id < best->id)) best = &in;
    }
    if (best) return use(*best);
    pick.state = MicrophonePick::NoneFound;
    return pick;
}

struct AudioDevices::Impl {
    AudioDevices *owner = nullptr;
    struct Node {
        Impl *impl = nullptr;
        uint32_t id = 0;
        pw_proxy *proxy = nullptr;
        spa_hook listener{};
    };

    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_registry *registry = nullptr;
    spa_hook registryListener{}, coreListener{};
    pw_metadata *metadata = nullptr;
    uint32_t metadataId = SPA_ID_INVALID;  // its global: gone when the session manager restarts
    spa_hook metadataListener{};
    std::map<uint32_t, std::unique_ptr<Node>> nodes;  // loop thread only
    Changed changed;

    mutable std::mutex mutex;
    std::vector<AudioNode> audioNodes;
    std::string defaultName, defaultSink;

    void notify() {
        if (changed) changed();
    }

    static void nodeInfo(void *data, const pw_node_info *info) {
        auto *n = static_cast<Node *>(data);
        if (!info || !(info->change_mask & PW_NODE_CHANGE_MASK_PROPS) || !info->props) return;
        std::map<std::string, std::string> props;
        const spa_dict_item *item;
        spa_dict_for_each(item, info->props) props[item->key] = item->value ? item->value : "";
        AudioNode in;
        const bool isInput = describeAudioNode(n->id, props, &in);
        {
            std::lock_guard<std::mutex> lock(n->impl->mutex);
            auto &list = n->impl->audioNodes;
            const auto old = std::find_if(list.begin(), list.end(), [&](const AudioNode &a) { return a.id == n->id; });
            if ((old == list.end() && !isInput) || (old != list.end() && isInput && *old == in)) return;
            list.erase(std::remove_if(list.begin(), list.end(), [&](const AudioInput &a) { return a.id == n->id; }),
                       list.end());
            if (isInput) list.push_back(in);
        }
        n->impl->notify();
    }

    static int metadataProperty(void *data, uint32_t subject, const char *key, const char *, const char *value) {
        auto *impl = static_cast<Impl *>(data);
        if (subject != PW_ID_CORE) return 0;
        const bool source = key && std::strcmp(key, "default.audio.source") == 0;
        const bool sink = key && std::strcmp(key, "default.audio.sink") == 0;
        if (key && !source && !sink) return 0;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (!key || source) impl->defaultName = key ? nameFromJson(value) : std::string();
            if (!key || sink) impl->defaultSink = key ? nameFromJson(value) : std::string();
        }
        impl->notify();
        return 0;
    }

    // The connection itself, on PipeWire's going away. What was found went with it.
    static void coreError(void *data, uint32_t id, int, int res, const char *) {
        auto *impl = static_cast<Impl *>(data);
        if (id != PW_ID_CORE || res != -EPIPE) return;
        impl->owner->m_connected.store(false);
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->audioNodes.clear();
            impl->defaultName.clear();
            impl->defaultSink.clear();
        }
        impl->notify();
    }

    static void global(void *data, uint32_t id, uint32_t, const char *type, uint32_t version, const spa_dict *props) {
        auto *impl = static_cast<Impl *>(data);
        if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
            const char *name = props ? spa_dict_lookup(props, PW_KEY_METADATA_NAME) : nullptr;
            if (impl->metadata || !name || std::strcmp(name, "default") != 0) return;
            impl->metadata = static_cast<pw_metadata *>(
                pw_registry_bind(impl->registry, id, type, std::min<uint32_t>(version, PW_VERSION_METADATA), 0));
            if (!impl->metadata) return;
            impl->metadataId = id;
            static const pw_metadata_events events = [] {
                pw_metadata_events e{};
                e.version = PW_VERSION_METADATA_EVENTS;
                e.property = metadataProperty;
                return e;
            }();
            pw_metadata_add_listener(impl->metadata, &impl->metadataListener, &events, impl);
            return;
        }
        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
        // The registry carries media.class; the full properties (device,
        // driver, virtual) come with the node's info. Playback streams are
        // included so applications can be captured without rerouting them.
        const char *cls = props ? spa_dict_lookup(props, PW_KEY_MEDIA_CLASS) : nullptr;
        if (!cls || (std::strncmp(cls, "Audio/", 6) != 0 && std::strcmp(cls, "Stream/Output/Audio") != 0))
            return;
        auto node = std::make_unique<Node>();
        node->impl = impl;
        node->id = id;
        node->proxy = static_cast<pw_proxy *>(
            pw_registry_bind(impl->registry, id, type, std::min<uint32_t>(version, PW_VERSION_NODE), 0));
        if (!node->proxy) return;
        static const pw_node_events events = [] {
            pw_node_events e{};
            e.version = PW_VERSION_NODE_EVENTS;
            e.info = nodeInfo;
            return e;
        }();
        pw_node_add_listener(reinterpret_cast<pw_node *>(node->proxy), &node->listener, &events, node.get());
        impl->nodes[id] = std::move(node);
    }

    static void globalRemove(void *data, uint32_t id) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->metadata && id == impl->metadataId) {
            // The session manager went (a restart): its next "default" is bound
            // when it comes, so the default devices are followed again.
            spa_hook_remove(&impl->metadataListener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy *>(impl->metadata));
            impl->metadata = nullptr;
            impl->metadataId = SPA_ID_INVALID;
            return;
        }
        const auto it = impl->nodes.find(id);
        if (it == impl->nodes.end()) return;
        spa_hook_remove(&it->second->listener);
        pw_proxy_destroy(it->second->proxy);
        impl->nodes.erase(it);
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            auto &list = impl->audioNodes;
            list.erase(std::remove_if(list.begin(), list.end(), [&](const AudioInput &a) { return a.id == id; }),
                       list.end());
        }
        impl->notify();
    }
};

AudioDevices::AudioDevices() : d(new Impl) {
    d->owner = this;
    pw_init(nullptr, nullptr);
}

AudioDevices::~AudioDevices() {
    stop();
    delete d;
}

bool AudioDevices::start(Changed changed, std::string *error) {
    stop();
    d->changed = std::move(changed);
    d->loop = pw_thread_loop_new("omabroadcast-devices", nullptr);
    d->context = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
    if (pw_thread_loop_start(d->loop) < 0) {
        if (error) *error = "PipeWire device loop did not start";
        stop();
        return false;
    }
    pw_thread_loop_lock(d->loop);
    d->core = pw_context_connect(d->context, nullptr, 0);
    if (!d->core) {
        pw_thread_loop_unlock(d->loop);
        if (error) *error = "No PipeWire server";
        stop();
        return false;
    }
    static const pw_core_events coreEvents = [] {
        pw_core_events e{};
        e.version = PW_VERSION_CORE_EVENTS;
        e.error = Impl::coreError;
        return e;
    }();
    pw_core_add_listener(d->core, &d->coreListener, &coreEvents, d);
    d->registry = pw_core_get_registry(d->core, PW_VERSION_REGISTRY, 0);
    static const pw_registry_events events = [] {
        pw_registry_events e{};
        e.version = PW_VERSION_REGISTRY_EVENTS;
        e.global = Impl::global;
        e.global_remove = Impl::globalRemove;
        return e;
    }();
    pw_registry_add_listener(d->registry, &d->registryListener, &events, d);
    m_connected.store(true);
    pw_thread_loop_unlock(d->loop);
    return true;
}

void AudioDevices::stop() {
    m_connected.store(false);
    if (!d->loop) return;
    pw_thread_loop_lock(d->loop);
    for (auto &[id, node] : d->nodes) {
        spa_hook_remove(&node->listener);
        pw_proxy_destroy(node->proxy);
    }
    d->nodes.clear();
    if (d->metadata) {
        spa_hook_remove(&d->metadataListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(d->metadata));
        d->metadata = nullptr;
    }
    d->metadataId = SPA_ID_INVALID;
    if (d->registry) {
        spa_hook_remove(&d->registryListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(d->registry));
        d->registry = nullptr;
    }
    if (d->core) {
        spa_hook_remove(&d->coreListener);
        pw_core_disconnect(d->core);
        d->core = nullptr;
    }
    pw_thread_loop_unlock(d->loop);
    pw_thread_loop_stop(d->loop);
    pw_context_destroy(d->context);
    d->context = nullptr;
    pw_thread_loop_destroy(d->loop);
    d->loop = nullptr;
    std::lock_guard<std::mutex> lock(d->mutex);
    d->audioNodes.clear();
    d->defaultName.clear();
    d->defaultSink.clear();
}

std::vector<AudioInput> AudioDevices::inputs() const {
    std::lock_guard<std::mutex> lock(d->mutex);
    std::vector<AudioInput> inputs;
    for (const AudioNode &node : d->audioNodes) if (node.kind == AudioNode::Input) inputs.push_back(node);
    return inputs;
}

std::string AudioDevices::defaultInput() const {
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->defaultName;
}

std::vector<AudioNode> AudioDevices::nodes() const {
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->audioNodes;
}

std::string AudioDevices::defaultOutput() const {
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->defaultSink;
}
