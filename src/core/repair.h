#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

// Help → Repair, apart from the window: finds studio collections and
// profiles that no longer read, moves files aside (never deleting them) into
// one dated folder under set-aside/ in the config folder, and finds capture
// grants whose collection or source is gone.
namespace repair {

struct Problem {
    QString kind;  // "collection" or "profile"
    QString id;    // the collection's file name, the profile's folder
    QString path;  // the file
    QString why;   // for people
};

bool collectionReadable(const QString &path, QString *why = nullptr);
// Collections (scenes/*.json) and profiles (profiles/*/profile.json) that do
// not read; a profile is judged by `profileReadable`, as the app loads them.
QList<Problem> check(const QString &configDir, const std::function<bool(const QString &path)> &profileReadable);

// The dated folder for what is set aside at `when`.
QString asideFolder(const QString &configDir, const QDateTime &when);
// Moves (or with `keep`, copies) a file under the config folder into `folder`,
// at the same place under it. A profile's folder left empty goes too.
bool setAside(const QString &configDir, const QString &folder, const QString &path, bool keep, QString *error);
// Writes `object` as `name` in `folder` (what a reset or a clear took away).
bool writeAside(const QString &folder, const QString &name, const QVariantMap &object, QString *error);

// The window's own preferences: docks, folded panels, the preview, the
// studio list, multiview and projectors. Behaviour (tray, confirmations) stays.
QStringList windowPreferenceKeys();

// Grant keys ("collection/source", or "source" in the Default collection)
// whose collection is gone or no longer has that capture source. The open
// collection counts as it is now: `current` is its file name, `currentCaptures`
// its capture sources.
QStringList staleGrants(const QVariantMap &grants, const QString &configDir, const QString &current,
                        const QStringList &currentCaptures);

}  // namespace repair
