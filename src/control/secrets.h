#pragma once

#include <QByteArray>
#include <QDBusConnection>
#include <QString>

#include <functional>

// Stream keys in the desktop keyring (the freedesktop Secret Service, which
// gnome-keyring and KWallet provide), never in a profile file. Each is found
// again by the stream's id. Calls block briefly; one that needs the keyring
// unlocked waits for the person to answer its prompt, running an event loop on
// the calling thread as it does. A copy can work on a thread of its own.
class SecretStore {
public:
    explicit SecretStore(QDBusConnection bus = QDBusConnection::sessionBus());

    // Whether a keyring answers on the session bus.
    bool available() const;
    bool store(const QString &id, const QString &label, const QByteArray &secret, QString *error = nullptr);
    // Empty with no error when there is none stored.
    QByteArray lookup(const QString &id, QString *error = nullptr);
    // Whether one is stored. Reads no secret, so it never asks to unlock, and
    // waits only a moment: false from a keyring that does not answer, with
    // `error` saying so, as that is not the same as none stored.
    bool has(const QString &id, QString *error = nullptr);
    bool remove(const QString &id, QString *error = nullptr);
    // When set: a prompt stops being waited for once this says no.
    std::function<bool()> stillWanted;

private:
    QString openSession(QString *error);
    // Runs a prompt the keyring asked for; false if it was dismissed.
    bool prompt(const QString &path, QString *error);
    QDBusConnection m_bus;
};
