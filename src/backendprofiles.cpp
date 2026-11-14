// Named output/audio setups. Collections and local capture grants stay with
// the open studio; exported profiles contain only portable settings.
#include "backend.h"
#include "core/repair.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace {
QString configDir() { return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation); }
QJsonObject readObject(const QString &path) {
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly) || f.size() > 1024*1024) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}
bool writeObject(const QString &path, const QJsonObject &object) {
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile f(path);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
}
// One of ours, or custom (its muxer is checked when it records).
bool knownContainer(const QString &id) {
    if (id == QLatin1String("custom")) return true;
    const auto &formats = fileFormats();
    return std::any_of(formats.begin(), formats.end(), [&](const FileFormat &f) { return id == QLatin1String(f.id); });
}

bool validProfile(const QJsonObject &object) {
    if (object.value("version").toInt() < 1 || object.value("version").toInt() > 3 ||
        !object.value("video").isObject() || !object.value("recording").isObject() || !object.value("audio").isObject()) return false;
    const auto video = object.value("video").toObject();
    auto integer = [](const QJsonObject &o, const char *key, int fallback, int low, int high) {
        if (!o.contains(key)) return true;
        const auto value = o.value(key);
        const double n = value.toDouble(fallback);
        return value.isDouble() && std::isfinite(n) && n == std::floor(n) && n >= low && n <= high;
    };
    if (!integer(video,"width",0,0,16384) || !integer(video,"height",0,0,16384) ||
        !integer(video,"fpsNum",60,1,240000) || !integer(video,"fpsDen",1,1,10000)) return false;
    if (video.value("width").toInt() == 1 || video.value("height").toInt() == 1) return false;
    const double rate = video.value("fpsNum").toDouble(60)/video.value("fpsDen").toDouble(1);
    if (rate < 1 || rate > 240) return false;
    const auto rec = object.value("recording").toObject();
    return integer(rec,"quality",23,0,51) && integer(rec,"bitrateKbps",0,0,1000000) &&
        integer(rec,"audioBitrateKbps",160,1,10000) &&
        knownContainer(rec.value("container").toString("mkv"));
}
QJsonObject portableProfile(const Profile &profile) {
    // Without what belongs to this machine and this account: its screen
    // grants, and stream keys and sign-ins (kept here without a keyring).
    Profile shared = profile;
    for (StreamTarget &t : shared.streams) { t.key.clear(); t.password.clear(); t.hasPassword = false; }
    auto settings = shared.toJson();
    settings.remove("restoreTokens"); settings.remove("collection");
    return {{"format", "omabroadcast-profile"}, {"version", 1}, {"profile", settings}};
}
}

bool Backend::profileFileReadable(const QString &path) { return validProfile(readObject(path)); }

bool Backend::profileError(const QString &message) { setStatus(message); emit failed(message); return false; }

void Backend::loadActiveProfile() {
    m_preferences = readObject(configDir()+"/global.json").value("preferences").toObject().toVariantMap();
    const auto selected = readObject(configDir()+"/global.json").value("activeProfile").toString("default");
    // Nothing saved yet: a fresh install, offered the set-up once.
    m_setupOffered = !QFile::exists(configDir()+"/global.json") && !QFile::exists(Profile::pathFor(selected)) &&
                     qEnvironmentVariableIsEmpty("OMABROADCAST_NO_SETUP_OFFER");
    // One that is there but does not read is set aside, never written over,
    // which the next save would do with stream targets and capture grants in it.
    const auto setAside = [this](const QString &id) {
        const QString path = Profile::pathFor(id);
        if (!QFile::exists(path)) return;
        const QString aside = repair::asideFolder(configDir(), QDateTime::currentDateTime());
        QString why;
        if (repair::setAside(configDir(), aside, path, false, &why))
            setStatus(tr("Profile %1 could not be read (damaged, or from a newer version); it was set aside in %2.").arg(id, aside));
        else
            qWarning("profile %s does not read and could not be set aside: %s", qPrintable(path), qPrintable(why));
    };
    // Ones saved before they were kept to this user: stream keys may be in them.
    for (const QString &id : QDir(configDir()+"/profiles").entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
        if (QFile::exists(Profile::pathFor(id))) QFile::setPermissions(Profile::pathFor(id), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    auto object = readObject(Profile::pathFor(selected));
    if (validProfile(object)) {
        m_profileId = selected;
    } else {
        setAside(selected);
        object = readObject(Profile::pathFor(m_profileId));
        if (!validProfile(object) && m_profileId != selected) setAside(m_profileId);
    }
    m_session.setProfile(validProfile(object) ? Profile::fromJson(object) : Profile{});
}

void Backend::setPreference(const QString &key, const QVariant &value) {
    if (m_preferences.value(key) == value) return;
    m_preferences.insert(key, value);
    auto global = readObject(configDir()+"/global.json");
    global.insert("preferences", QJsonObject::fromVariantMap(m_preferences));
    // Only this user reads it: it holds the remote control password.
    if (!writeObject(configDir()+"/global.json", global)) setStatus(tr("Could not save preferences."));
    else QFile::setPermissions(configDir()+"/global.json", QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    emit preferencesChanged();
}

void Backend::setHideFromCapture(bool hide) {
    setPreference(QStringLiteral("hideFromCapture"), hide);
    applyCaptureHiding();
}

QVariantList Backend::gpuChoices() const {
    if (!m_gpuChoices.isEmpty()) return m_gpuChoices;
    m_gpuChoices << QVariantMap{{QStringLiteral("value"), QStringLiteral("auto")}, {QStringLiteral("label"), tr("Automatic")}};
    const std::vector<engine::GpuDevice> gpus = engine::listGpus();
    for (const engine::GpuDevice &g : gpus) {
        if (!g.usable || g.id.empty()) continue;
        using Kind = engine::GpuDevice::Kind;
        const QString kind = g.kind == Kind::Discrete ? tr("discrete") : g.kind == Kind::Integrated ? tr("integrated") : tr("virtual");
        QString name = QString::fromStdString(g.name);
        // Two of the same card: tell them apart by where they sit.
        if (std::count_if(gpus.begin(), gpus.end(), [&g](const engine::GpuDevice &o) { return o.name == g.name; }) > 1 && !g.renderNode.empty())
            name += QStringLiteral(" ") + QString::fromStdString(g.renderNode);
        m_gpuChoices << QVariantMap{{QStringLiteral("value"), QString::fromStdString(g.id)},
                                    {QStringLiteral("label"), tr("%1 (%2)").arg(name, kind)}};
    }
    m_gpuChoices << QVariantMap{{QStringLiteral("value"), QStringLiteral("cpu")}, {QStringLiteral("label"), tr("Processor only (slow)")}};
    return m_gpuChoices;
}

void Backend::setGpu(const QString &choice) {
    if (choice == gpu()) return;
    QString label;
    for (const QVariant &v : gpuChoices())
        if (v.toMap().value(QStringLiteral("value")).toString() == choice) label = v.toMap().value(QStringLiteral("label")).toString();
    if (label.isEmpty()) return;
    // The picture restarts on the new GPU, which would end every output.
    if (outputsBusy()) {
        setStatus(tr("Stop recording, streaming, the replay buffer and the virtual camera before changing the GPU."));
        emit preferencesChanged();  // the box goes back to the one in use
        return;
    }
    setPreference(QStringLiteral("gpu"), choice);
    m_session.setGpuChoice(choice);
    m_pendingRestart.clear();
    if (m_session.running()) restartSession();
    setStatus(tr("Graphics: %1.").arg(label));
}

void Backend::applyCaptureHiding() {
    if (!captureHidingAvailable()) return;
    if (!m_hyprland.setHidden(hideFromCapture())) setStatus(tr("Hyprland did not accept the capture rule."));
}

QVariantList Backend::profiles() const {
    QVariantList out;
    const auto entries = QDir(configDir()+"/profiles").entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    for (const auto &id : entries) {
        const auto object = readObject(Profile::pathFor(id));
        if (id != m_profileId && !validProfile(object)) continue;
        const auto name = id == m_profileId ? profileName() : object.value("name").toString(id == "default" ? tr("Default") : id);
        out.append(QVariantMap{{"id",id},{"name",name},{"current",id == m_profileId}});
    }
    if (std::none_of(out.cbegin(),out.cend(),[&](const QVariant &v){ return v.toMap().value("current").toBool(); }))
        out.append(QVariantMap{{"id",m_profileId},{"name",profileName()},{"current",true}});
    std::sort(out.begin(),out.end(),[](const QVariant &a,const QVariant &b) {
        return QString::localeAwareCompare(a.toMap().value("name").toString(),b.toMap().value("name").toString()) < 0;
    });
    return out;
}

bool Backend::profileChangeAllowed() {
    // Switching restarts the session, which would end every output.
    return !outputsBusy() ? true
                          : profileError(tr("Stop recording, streaming, the replay buffer and the virtual camera, and let them finish, before changing profiles."));
}

QString Backend::uniqueProfileName(const QString &name) const {
    const auto base = name.trimmed().isEmpty() ? tr("Profile") : name.trimmed().left(120);
    QStringList used;
    for (const auto &v : profiles()) used << v.toMap().value("name").toString();
    auto candidate = base;
    for (int n=2; used.contains(candidate,Qt::CaseInsensitive); ++n) candidate = tr("%1 %2").arg(base).arg(n);
    return candidate;
}

bool Backend::activateProfile(const QString &id, Profile profile) {
    if (!profileChangeAllowed()) return false;
    if (!saveProfile()) return profileError(tr("Could not save the current profile."));
    // Profiles are independent of collections. Keep the open collection and
    // its local portal grants, including any updated restore tokens.
    profile.collection = m_session.profile().collection;
    profile.restoreTokens = m_session.profile().restoreTokens;
    profile.legacySources.clear();
    const bool existed = QFileInfo::exists(Profile::pathFor(id));
    if (!profile.save(id)) return profileError(tr("Could not save the selected profile."));
    auto global = readObject(configDir()+"/global.json");
    global["activeProfile"] = id;
    if (!writeObject(configDir()+"/global.json",global)) {
        // An unsuccessful New/Import must not leave an invisible extra setup.
        if (!existed) {
            QFile::remove(Profile::pathFor(id));
            QDir().rmdir(QFileInfo(Profile::pathFor(id)).absolutePath());
        }
        return profileError(tr("Could not remember the selected profile."));
    }
    const bool running = m_session.running();
    // Frame rate is fixed for each render loop. Quiesce it before replacing
    // video/audio settings, and reopen capture using the retained local grants.
    m_audioHotkeys.stopGlobal();
    m_session.cancelAudioPush({},~0u);
    m_session.stop();
    m_profileId = id;
    m_session.setProfile(profile);
    // The layout goes with the collection, so it follows this profile's canvas.
    fitLayoutToCanvas();
    m_desktopMeter.clear(); m_microphoneMeter.clear(); m_sourceAudioMeters.clear(); m_globalAudioDeviceMeters.clear();
    if (running) m_session.start();
    fitLayoutToCanvas();
    refreshHotkeys();
    // Safe mode asks the desktop for nothing.
    if (running && profile.globalHotkeys && !safeMode()) m_audioHotkeys.bindGlobal();
    emit profilesChanged(); emit settingsChanged(); emit microphonesChanged(); emit levelsChanged(); emit audioPushChanged();
    setStatus(tr("Profile: %1").arg(profile.name));
    return true;
}

bool Backend::switchProfile(const QString &id) {
    if (id == m_profileId) return true;
    if (!profileChangeAllowed()) return false;
    // IDs are directory components, never paths supplied by an import.
    if (QFileInfo(QFileInfo(Profile::pathFor(id)).absolutePath()).isSymLink()) return profileError(tr("That profile is not available."));
    const auto object = readObject(Profile::pathFor(id));
    if (!validProfile(object)) return profileError(tr("That profile is missing or invalid."));
    return activateProfile(id,Profile::fromJson(object));
}

bool Backend::createProfile(Profile profile, const QString &name) {
    if (!profileChangeAllowed()) return false;
    profile.name = uniqueProfileName(name);
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return activateProfile(id,profile);
}
bool Backend::newProfile(const QString &name) {
    // At the canvas in use, so nothing moves. One still waiting for its first
    // capture stays automatic.
    Profile fresh;
    fresh.canvasWidth = m_session.profile().canvasWidth;
    fresh.canvasHeight = m_session.profile().canvasHeight;
    return createProfile(fresh,name);
}

// A secret kept under one id, kept under another too. Fine when there is none.
bool Backend::copySecret(const QString &from, const QString &to, const QString &label, QString *why) {
    const bool there = m_secrets.has(from, why);
    if (!why->isEmpty()) return false;  // no answer is not "none"
    if (!there) return true;
    const QByteArray secret = m_secrets.lookup(from, why);
    if (!why->isEmpty()) return false;
    return secret.isEmpty() || m_secrets.store(to, label, secret, why);
}

bool Backend::duplicateProfile() {
    if (!profileChangeAllowed()) return false;
    // Stream keys and passwords are found by the stream's id: the copy's streams
    // have their own, and their own copies of those, so that changing or removing
    // one leaves the other profile's alone.
    Profile copy = m_session.profile();
    QStringList stored;
    QString why;
    for (StreamTarget &t : copy.streams) {
        const QString from = t.id;
        t.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!m_secrets.available()) continue;
        const QString password = t.id + QStringLiteral("/password");
        stored << t.id << password;
        if (copySecret(from, t.id, streamKeyLabel(t), &why) &&
            copySecret(from + QStringLiteral("/password"), password, QStringLiteral("OmaBroadcast stream password (%1)").arg(t.name.isEmpty() ? t.server : t.name), &why)) continue;
        for (const QString &id : std::as_const(stored)) m_secrets.remove(id);
        return profileError(tr("The profile was not copied, as the keyring did not give its stream key: %1").arg(why));
    }
    return createProfile(copy,tr("%1 copy").arg(profileName()));
}

bool Backend::renameProfile(const QString &name) {
    if (!profileChangeAllowed()) return false;
    const auto wanted = name.trimmed().left(120);
    if (wanted.isEmpty()) return profileError(tr("Enter a profile name."));
    if (wanted == profileName()) return true;
    for (const auto &v : profiles()) {
        const auto entry = v.toMap();
        if (!entry.value("current").toBool() && entry.value("name").toString().compare(wanted,Qt::CaseInsensitive) == 0)
            return profileError(tr("A profile already has that name."));
    }
    auto changed = m_session.profile(); changed.name = wanted;
    if (!changed.save(m_profileId)) return profileError(tr("Could not rename the profile."));
    m_session.profile().name = wanted;
    emit profilesChanged(); return true;
}

bool Backend::removeProfile(const QString &id) {
    if (!profileChangeAllowed()) return false;
    const auto all = profiles();
    if (all.size() < 2) return profileError(tr("Keep at least one profile."));
    bool found = false;
    for (const auto &v : all) if (v.toMap().value("id").toString() == id) found = true;
    if (!found) return profileError(tr("That profile is not available."));
    if (id == m_profileId) {
        for (const auto &v : all) {
            const auto other = v.toMap().value("id").toString();
            if (other != id) { if (!switchProfile(other)) return false; break; }
        }
    }
    // A new account may have no data folder yet, and the bin is made inside it, not with it.
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    if (!QFile::moveToTrash(QFileInfo(Profile::pathFor(id)).absolutePath()))
        return profileError(tr("Could not move the profile to the bin. It has been kept."));
    emit profilesChanged(); return true;
}

bool Backend::importProfileFrom(const QString &path) {
    if (!profileChangeAllowed()) return false;
    auto object = readObject(path);
    if (object.contains("format")) {
        if (object.value("format").toString() != "omabroadcast-profile" || object.value("version").toInt() != 1)
            return profileError(tr("That profile export is not supported."));
        object = object.value("profile").toObject();
    }
    if (!validProfile(object)) return profileError(tr("That is not a valid OmaBroadcast profile."));
    auto profile = Profile::fromJson(object);
    profile.restoreTokens.clear(); profile.legacySources.clear();
    // Streams of its own: importing a profile's own export must not share its
    // stream keys, and a password kept in a keyring did not come with the file.
    for (StreamTarget &t : profile.streams) {
        t.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        t.hasPassword = !t.password.isEmpty();
    }
    return createProfile(profile,object.value("name").toString(QFileInfo(path).completeBaseName()));
}

bool Backend::exportProfileTo(const QString &path) {
    return writeObject(path,portableProfile(m_session.profile())) || profileError(tr("Could not export the profile."));
}
void Backend::importProfile() {
    if (!profileChangeAllowed()) return;
    m_otherChosen = [this](const QString &path) { importProfileFrom(path); };
    m_otherPicker.openFile(tr("Import a profile"),tr("Profiles"),{"*.json"});
}
void Backend::exportProfile() {
    const auto snapshot = portableProfile(m_session.profile());
    m_otherChosen = [this,snapshot](const QString &path) {
        const QString target = exportFile(path);
        if (target.isEmpty()) return;
        if (writeObject(target,snapshot)) setStatus(tr("Profile exported to %1").arg(target));
        else profileError(tr("Could not export the profile."));
    };
    auto filename = profileName();
    filename.replace('/','-'); filename.replace('\\','-');
    m_otherPicker.saveFile(tr("Export the profile"),filename+".json",tr("Profiles"),{"*.json"});
}
