#include "repair.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

namespace repair {

namespace {
QString tr(const char *text) { return QCoreApplication::translate("Repair", text); }

bool isCapture(const QString &kind) {
    return kind == QLatin1String("display") || kind == QLatin1String("window") || kind == QLatin1String("region");
}

// A collection's capture sources, or false when it does not read.
bool captureSources(const QString &path, QStringList *ids) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    for (const QJsonValue &v : doc.object().value(QStringLiteral("sources")).toArray())
        if (isCapture(v.toObject().value(QStringLiteral("kind")).toString())) ids->append(v.toObject().value(QStringLiteral("id")).toString());
    return true;
}
}  // namespace

bool collectionReadable(const QString &path, QString *why) {
    const auto fail = [why](const QString &text) { if (why) *why = text; return false; };
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(tr("cannot be opened"));
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &error);
    if (error.error != QJsonParseError::NoError) return fail(tr("damaged at character %1").arg(error.offset));
    if (!doc.isObject()) return fail(tr("not a studio collection"));
    const QJsonObject o = doc.object();
    if (!o.value(QStringLiteral("scenes")).isArray()) return fail(tr("has no studios"));
    if (o.contains(QStringLiteral("sources")) && !o.value(QStringLiteral("sources")).isArray()) return fail(tr("its sources do not read"));
    return true;
}

QList<Problem> check(const QString &configDir, const std::function<bool(const QString &path)> &profileReadable) {
    QList<Problem> out;
    const QDir scenes(configDir + QStringLiteral("/scenes"));
    for (const QFileInfo &fi : scenes.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name | QDir::IgnoreCase)) {
        QString why;
        if (!collectionReadable(fi.absoluteFilePath(), &why))
            out.append({QStringLiteral("collection"), fi.completeBaseName(), fi.absoluteFilePath(), why});
    }
    const QDir profiles(configDir + QStringLiteral("/profiles"));
    for (const QString &id : profiles.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
        const QString path = profiles.filePath(id) + QStringLiteral("/profile.json");
        if (QFile::exists(path) && !profileReadable(path))
            out.append({QStringLiteral("profile"), id, path, tr("damaged or from a newer version")});
    }
    return out;
}

QString asideFolder(const QString &configDir, const QDateTime &when) {
    return configDir + QStringLiteral("/set-aside/") + when.toString(QStringLiteral("yyyy-MM-dd HH-mm-ss"));
}

bool setAside(const QString &configDir, const QString &folder, const QString &path, bool keep, QString *error) {
    const auto fail = [error](const QString &text) { if (error) *error = text; return false; };
    const QString relative = QDir(configDir).relativeFilePath(path);
    if (relative.startsWith(QLatin1String("..")) || QFileInfo(relative).isAbsolute()) return fail(tr("%1 is not in the settings folder").arg(path));
    QString target = folder + QLatin1Char('/') + relative;
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) return fail(tr("could not make %1").arg(QFileInfo(target).absolutePath()));
    // Never over something set aside before.
    for (int n = 2; QFileInfo::exists(target); ++n)
        target = folder + QLatin1Char('/') + relative + QStringLiteral(".%1").arg(n);
    const bool done = keep ? QFile::copy(path, target) : QFile::rename(path, target);
    if (!done) return fail(tr("could not move %1 to %2").arg(path, target));
    if (!keep && QFileInfo(path).fileName() == QLatin1String("profile.json"))
        QDir().rmdir(QFileInfo(path).absolutePath());  // only when empty
    return true;
}

bool writeAside(const QString &folder, const QString &name, const QVariantMap &object, QString *error) {
    if (!QDir().mkpath(folder)) { if (error) *error = tr("could not make %1").arg(folder); return false; }
    QSaveFile f(folder + QLatin1Char('/') + name);
    const QByteArray bytes = QJsonDocument(QJsonObject::fromVariantMap(object)).toJson(QJsonDocument::Indented);
    if (f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit()) return true;
    if (error) *error = tr("could not write %1").arg(f.fileName());
    return false;
}

QStringList windowPreferenceKeys() {
    return {QStringLiteral("docks"), QStringLiteral("dockSizes"), QStringLiteral("docksLocked"), QStringLiteral("panels"),
            QStringLiteral("previewLocked"), QStringLiteral("showOverflow"), QStringLiteral("contextBar"), QStringLiteral("studioGrid"),
            QStringLiteral("multiviewLayout"), QStringLiteral("multiviewLabels"), QStringLiteral("multiviewSafeAreas"),
            QStringLiteral("multiviewClickTakes"), QStringLiteral("projectors")};
}

QStringList staleGrants(const QVariantMap &grants, const QString &configDir, const QString &current,
                        const QStringList &currentCaptures) {
    QHash<QString, QStringList> captures;  // by collection; absent when it does not read
    QSet<QString> unreadable;
    QStringList out;
    for (auto it = grants.cbegin(); it != grants.cend(); ++it) {
        const qsizetype slash = it.key().indexOf(QLatin1Char('/'));
        const QString collection = slash < 0 ? QStringLiteral("Default") : it.key().left(slash);
        const QString source = slash < 0 ? it.key() : it.key().mid(slash + 1);
        if (collection == current) {
            if (!currentCaptures.contains(source)) out << it.key();
            continue;
        }
        if (!captures.contains(collection) && !unreadable.contains(collection)) {
            QStringList ids;
            if (captureSources(configDir + QStringLiteral("/scenes/") + collection + QStringLiteral(".json"), &ids)) captures.insert(collection, ids);
            else unreadable.insert(collection);
        }
        if (unreadable.contains(collection) || !captures.value(collection).contains(source)) out << it.key();
    }
    return out;
}

}  // namespace repair
