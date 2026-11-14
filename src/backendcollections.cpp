// Scene collections: several of them, import and export, and finding the
// files a collection uses when they have moved.

#include "backend.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

QString collectionsDir() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/scenes");
}

// A collection's name as it is kept on disk, and not already taken.
QString freeFileName(const QString &name) {
    QString base = name;
    base.replace(QLatin1Char('/'), QLatin1Char('-'));
    while (base.startsWith(QLatin1Char('.'))) base.remove(0, 1);
    if (base.trimmed().isEmpty()) base = QStringLiteral("Collection");
    QString candidate = base;
    for (int n = 2; QFile::exists(collectionsDir() + QLatin1Char('/') + candidate + QStringLiteral(".json")); ++n)
        candidate = QStringLiteral("%1 %2").arg(base).arg(n);
    return candidate;
}

// The setting that names a file or folder on disk, for each kind that has one.
QString fileKey(const SourceDef &s) {
    if (s.kind == QLatin1String("image")) return QStringLiteral("path");
    if (s.kind == QLatin1String("slideshow")) return s.settings.contains(QStringLiteral("folder")) ? QStringLiteral("folder") : QString();
    if (s.kind == QLatin1String("text")) return QStringLiteral("file");
    if (s.kind == QLatin1String("browser")) return s.settings.value(QStringLiteral("local")).toBool() ? QStringLiteral("path") : QString();
    if (s.kind == QLatin1String("media")) {
        // A network stream listens; a URL is not a file.
        if (s.settings.contains(QStringLiteral("port"))) return {};
        if (s.settings.value(QStringLiteral("input")).toString().contains(QStringLiteral("://"))) return {};
        return QStringLiteral("input");
    }
    return {};
}

// Match by name and kind only when there is one candidate. A shared path can
// have many references, while two different candidates need an explicit choice.
QHash<int, QString> missingMatches(const QVariantList &missing, const QString &folder, bool recursive) {
    if (missing.isEmpty()) return {};
    QHash<QString, QStringList> candidates;
    auto key = [](const QString &path, bool directory) { return (directory ? "d:" : "f:") + QFileInfo(path).fileName(); };
    for (const auto &value : missing) { const auto ref = value.toMap(); candidates.insert(key(ref.value("path").toString(), ref.value("folder").toBool()), {}); }
    auto collect = [&](const QFileInfo &file) {
        auto found = candidates.find(key(file.filePath(), file.isDir()));
        if (found != candidates.end() && (file.isDir() || file.isFile()) && !found->contains(file.absoluteFilePath()))
            found->append(file.absoluteFilePath());
    };
    collect(QFileInfo(folder));
    QDirIterator it(folder, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
                    recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    int seen = 0;
    while (it.hasNext() && seen++ < 200000) collect(QFileInfo(it.next()));
    if (it.hasNext()) return {}; // an incomplete search cannot establish uniqueness
    QHash<int, QString> matches;
    for (int i = 0; i < missing.size(); ++i) {
        const auto ref = missing[i].toMap(); const auto paths = candidates.value(key(ref.value("path").toString(), ref.value("folder").toBool()));
        if (paths.size() == 1) matches.insert(i, paths.first());
    }
    return matches;
}

QHash<QString, QVariantMap> relinkSettings(const Collection &collection, const QVariantList &missing,
                                         const QHash<int, QString> &matches) {
    QHash<QString, QVariantMap> changes;
    for (auto it = matches.begin(); it != matches.end(); ++it) {
        const auto ref = missing[it.key()].toMap(); const auto id = ref.value("sourceId").toString();
        const auto *source = collection.source(id); if (!source) continue;
        auto settings = changes.value(id, source->settings);
        const auto key = ref.value("key").toString(); const int index = ref.value("index", -1).toInt();
        if (key == "entries") {
            auto entries = slideshowEntries(settings); if (index < 0 || index >= entries.size()) continue;
            auto entry = entries[index].toMap(); entry["path"] = it.value(); entries[index] = entry; settings[key] = entries;
        } else if (key == "files") {
            auto files = settings.value(key).toStringList(); if (index < 0 || index >= files.size()) continue;
            files[index] = it.value(); settings[key] = files;
        } else settings[key] = it.value();
        changes[id] = settings;
    }
    return changes;
}

bool writeCollection(const QString &path, const Collection &c) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    if (c.holdsPassphrase()) f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QJsonDocument(c.toJson()).toJson(QJsonDocument::Indented));
    return f.commit();
}

}  // namespace

QString Backend::collectionFile(const QString &base) const {
    return collectionsDir() + QLatin1Char('/') + base + QStringLiteral(".json");
}

QVariantList Backend::collections() const {
    QVariantList out;
    const QString current = m_session.profile().collection;
    const QFileInfoList files = QDir(collectionsDir()).entryInfoList({QStringLiteral("*.json")}, QDir::Files,
                                                                     QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &fi : files) {
        const QString base = fi.completeBaseName();
        QString name = base;
        if (base == current) {
            name = m_production.collection().name;
        } else {
            QFile f(fi.absoluteFilePath());
            if (f.open(QIODevice::ReadOnly))
                name = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("name")).toString(base);
        }
        out.append(QVariantMap{{QStringLiteral("file"), base}, {QStringLiteral("name"), name},
                               {QStringLiteral("current"), base == current}});
    }
    return out;
}

QString Backend::collectionName() const { return m_production.collection().name; }

bool Backend::openCollection(const QString &base) {
    // What is open now, as it is. Edits that cannot be written stay open with it,
    // as going on would lose them.
    if (!m_production.path().isEmpty() && !m_production.save() && m_production.unsaved()) {
        setStatus(tr("Could not save the studios to %1, so they stay open. Change collection once they can be saved.").arg(m_production.path()));
        emit failed(status());
        emit collectionsChanged();
        return false;
    }
    Profile &profile = m_session.profile();
    profile.collection = base;
    // Default keeps the keys it had before there were other collections.
    m_session.setTokenScope(base == QLatin1String("Default") ? QString() : base);
    saveProfile();
    loadCollection();
    dropGoneProjectors();
    fitLayoutToCanvas();
    m_session.restartFeeds();
    emit collectionsChanged();
    return true;
}

void Backend::switchCollection(const QString &base) {
    if (base == m_session.profile().collection || !QFile::exists(collectionFile(base))) return;
    openCollection(base);
}

void Backend::newCollection(const QString &name) {
    Collection c;
    c.name = name.trimmed().isEmpty() ? tr("Collection") : name.trimmed();
    Scene scene{c.makeId(QLatin1Char('c')), tr("Studio 1"), {}};
    c.scenes.append(scene);
    c.currentScene = scene.id;
    createCollection(c);
}

bool Backend::createCollection(const Collection &c) {
    const QString base = freeFileName(c.name);
    if (!writeCollection(collectionFile(base), c)) {
        setStatus(tr("Could not make the collection."));
        return false;
    }
    if (openCollection(base)) return true;
    QFile::remove(collectionFile(base));  // what is open stayed open: no new one after all
    emit collectionsChanged();
    return false;
}

void Backend::duplicateCollection() {
    Collection c = m_production.collection();
    c.name = tr("%1 copy").arg(c.name);
    const QString base = freeFileName(c.name);
    if (!writeCollection(collectionFile(base), c)) {
        setStatus(tr("Could not copy the collection."));
        return;
    }
    if (!openCollection(base)) {
        QFile::remove(collectionFile(base));
        emit collectionsChanged();
    }
}

void Backend::renameCollection(const QString &name) {
    if (name.trimmed().isEmpty()) return;
    m_production.setName(name.trimmed());
    m_production.save();
    emit collectionsChanged();
}

void Backend::removeCollection(const QString &base) {
    const QVariantList all = collections();
    if (all.size() < 2) return;  // there is always one
    if (base == m_session.profile().collection) {
        for (const QVariant &v : all) {
            const QString other = v.toMap().value(QStringLiteral("file")).toString();
            if (other != base) {
                if (!openCollection(other)) return;  // still open: it stays
                break;
            }
        }
    }
    // To the bin, not gone: a removed collection can be fished back out.
    // A new account may have no data folder yet, and the bin is made inside it, not with it.
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    if (!QFile::moveToTrash(collectionFile(base))) QFile::remove(collectionFile(base));
    // Its screen and window grants go with it: ids start again, so a new collection
    // of this name would have its first source shown the old one's window unasked.
    QVariantMap &grants = m_session.profile().restoreTokens;
    const bool unscoped = base == QLatin1String("Default");  // see setTokenScope in openCollection
    for (auto it = grants.begin(); it != grants.end();)
        it = (unscoped ? !it.key().contains(QLatin1Char('/')) : it.key().startsWith(base + QLatin1Char('/'))) ? grants.erase(it) : std::next(it);
    saveProfile();
    dropGoneProjectors();
    emit collectionsChanged();
}

void Backend::importCollection() {
    m_otherChosen = [this](const QString &path) { importCollectionFrom(path); };
    m_otherPicker.openFile(tr("Import a collection"), tr("Collections"), {QStringLiteral("*.json")});
}

void Backend::importCollectionFrom(const QString &path) {
    {
        QFile f(path);
        const QJsonDocument doc = f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()) : QJsonDocument();
        if (!doc.isObject() || !doc.object().value(QStringLiteral("scenes")).isArray()) {
            setStatus(tr("That is not an OmaBroadcast collection."));
            return;
        }
        Collection c = Collection::fromJson(doc.object());
        c.prune();
        if (c.scenes.isEmpty()) {
            setStatus(tr("That collection has no studios."));
            return;
        }
        if (!c.scene(c.currentScene) || c.scene(c.currentScene)->group)
            for (const Scene &s : c.scenes)
                if (!s.group) {
                    c.currentScene = s.id;
                    break;
                }
        // Its own name, made unique among what is here.
        QStringList names;
        for (const QVariant &v : collections()) names << v.toMap().value(QStringLiteral("name")).toString();
        const QString wanted = c.name;
        for (int n = 2; names.contains(c.name); ++n) c.name = QStringLiteral("%1 %2").arg(wanted).arg(n);
        const QString base = freeFileName(c.name);
        if (!writeCollection(collectionFile(base), c)) {
            setStatus(tr("Could not import the collection."));
            return;
        }
        if (!openCollection(base)) {
            QFile::remove(collectionFile(base));
            emit collectionsChanged();
            return;
        }
        if (const int missing = int(missingFiles().size()); missing > 0)
            setStatus(missing == 1 ? tr("Imported. A file it uses is not on this computer: use Find missing files.")
                                   : tr("Imported. %1 files it uses are not on this computer: use Find missing files.").arg(missing));
    }
}

void Backend::exportCollection() {
    m_otherChosen = [this](const QString &path) {
        const QString target = exportFile(path);
        if (target.isEmpty()) return;
        setStatus(writeCollection(target, m_production.collection().withoutPassphrases()) ? tr("Exported to %1").arg(target)
                                                                     : tr("Could not export to %1").arg(target));
    };
    m_otherPicker.saveFile(tr("Export the collection"), m_production.collection().name + QStringLiteral(".json"),
                           tr("Collections"), {QStringLiteral("*.json")});
}

QString Backend::exportFile(const QString &picked) {
    if (picked.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) return picked;
    const QString target = picked + QStringLiteral(".json");
    if (!QFileInfo::exists(target)) return target;
    setStatus(tr("%1 is there already and was left alone. To replace it, choose it by its full name.").arg(target));
    emit failed(status());
    return {};
}

// ------------------------------------------------------------ missing files

QVariantList Backend::missingFiles() const {
    QVariantList asked;
    QStringList paths;
    QList<bool> folders;
    for (const SourceDef &source : m_production.collection().sources) {
        auto append = [&](const QString &key, const QString &path, bool folder, int index = -1) {
            if (path.isEmpty()) return;
            paths << path;
            folders << folder;
            asked << QVariantMap{{"sourceId", source.id}, {"name", source.name}, {"kind", source.kind},
                                 {"key", key}, {"path", path}, {"folder", folder}, {"index", index}};
        };
        if (source.kind == "slideshow" && source.settings.contains("entries")) {
            const auto entries = slideshowEntries(source.settings);
            for (int index = 0; index < entries.size(); ++index) {
                const auto entry = entries[index].toMap(); append("entries", entry.value("path").toString(), entry.value("folder").toBool(), index);
            }
        } else if (source.kind == "slideshow" && source.settings.value("folder").toString().isEmpty()) {
            const auto files = source.settings.value("files").toStringList();
            for (int index = 0; index < files.size(); ++index) append("files", files[index], false, index);
        } else {
            const auto key = fileKey(source); if (!key.isEmpty()) append(key, source.settings.value(key).toString(), key == "folder");
        }
    }
    // The window asks this whenever anything changes. With a mount gone the
    // last answer stands: the question would wait for ever.
    QList<bool> there;
    if (!pathsThere(paths, folders, &there)) return m_lastMissing;
    QVariantList out;
    for (qsizetype i = 0; i < asked.size(); ++i)
        if (!there[i]) out << asked[i];
    m_lastMissing = out;
    return out;
}

int Backend::relinkFrom(const QString &folder, bool recursive) {
    const auto missing = missingFiles();
    const auto matches = missingMatches(missing, folder, recursive);
    const auto changes = relinkSettings(m_production.collection(), missing, matches);
    if (!changes.isEmpty()) m_production.setSourcesSettings(changes, tr("Find missing files"));
    return matches.size();
}

int Backend::relinkMissingTo(const QString &sourceId, const QString &key, int index,
                            const QString &expectedPath, const QString &replacement) {
    const auto missing = missingFiles(); QVariantMap chosen;
    for (const auto &value : missing) {
        const auto ref = value.toMap();
        if (ref.value("sourceId").toString() == sourceId && ref.value("key").toString() == key &&
            ref.value("index").toInt() == index && ref.value("path").toString() == expectedPath) { chosen = ref; break; }
    }
    if (chosen.isEmpty()) return 0; // the entry changed while its picker was open
    const QFileInfo file(replacement); const bool directory = chosen.value("folder").toBool();
    if (directory ? !file.isDir() : !file.isFile()) return 0;
    auto matches = missingMatches(missing, file.absolutePath(), false);
    for (int i = 0; i < missing.size(); ++i) {
        const auto ref = missing[i].toMap();
        if (ref.value("path").toString() == expectedPath && ref.value("folder").toBool() == directory)
            matches[i] = file.absoluteFilePath();
    }
    const auto changes = relinkSettings(m_production.collection(), missing, matches);
    if (!changes.isEmpty()) m_production.setSourcesSettings(changes, tr("Find missing files"));
    return matches.size();
}

void Backend::locateMissing(const QString &sourceId, int index, const QString &key) {
    QVariantMap chosen;
    for (const auto &value : missingFiles()) {
        const auto ref = value.toMap();
        if (ref.value("sourceId").toString() == sourceId &&
            ((key.isEmpty() && index < 0) || (ref.value("index").toInt() == index && (key.isEmpty() || ref.value("key").toString() == key)))) { chosen = ref; break; }
    }
    if (chosen.isEmpty()) return;
    const auto generation = m_production.generation();
    m_otherChosen = [this, chosen, generation](const QString &path) {
        if (generation != m_production.generation()) return;
        const int found = relinkMissingTo(chosen.value("sourceId").toString(), chosen.value("key").toString(),
            chosen.value("index").toInt(), chosen.value("path").toString(), path);
        if (found > 1) setStatus(tr("Relinked %1 missing references.").arg(found));
    };
    const auto title = tr("Where is %1 now?").arg(QFileInfo(chosen.value("path").toString()).fileName());
    if (chosen.value("folder").toBool()) m_otherPicker.openFolder(title); else m_otherPicker.openFile(title);
}

void Backend::searchForMissing() {
    m_otherChosen = [this](const QString &folder) {
        const int before = int(missingFiles().size());
        const int found = relinkFrom(folder, true);
        setStatus(found == 0 ? tr("None of the missing files are in %1.").arg(folder)
                             : tr("Found %1 of %2.").arg(found).arg(before));
    };
    m_otherPicker.openFolder(tr("Search a folder for the missing files"));
}
