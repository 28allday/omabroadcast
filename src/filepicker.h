#pragma once

#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

// The XDG desktop portal file chooser. Under Wayland this is the only picker
// that looks and behaves like the rest of the desktop — never QFileDialog.
//
// Requests return immediately; answers arrive as selected/filesSelected or canceled.
class PortalFileChooser : public QObject {
    Q_OBJECT

public:
    explicit PortalFileChooser(QObject *parent = nullptr);
    ~PortalFileChooser() override;
    void cancel();

    // `patterns` are globs, e.g. {"*.md", "*.markdown"}; empty means any file.
    Q_INVOKABLE void openFile(const QString &title, const QString &filterName = QString(),
                              const QStringList &patterns = {});
    Q_INVOKABLE void openFiles(const QString &title, const QString &filterName = QString(),
                               const QStringList &patterns = {});
    // A folder rather than a file; `startIn`, when it exists, is where the picker opens.
    Q_INVOKABLE void openFolder(const QString &title, const QString &startIn = QString());
    Q_INVOKABLE void saveFile(const QString &title, const QString &suggestedName,
                              const QString &filterName = QString(),
                              const QStringList &patterns = {});

signals:
    void selected(const QUrl &url);
    void filesSelected(const QList<QUrl> &urls);
    void canceled();
    void failed(const QString &message);

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    bool request(const QString &method, const QString &title, QVariantMap options);
    void clearPending(bool close = false);

    QString m_pendingPath;
    bool m_multiple = false;
};
