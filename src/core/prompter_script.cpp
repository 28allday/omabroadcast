#include "core/prompter_script.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>
#include <QTextDocument>

namespace prompter {

namespace {
QString said(const char *text) { return QCoreApplication::translate("Prompter", text); }
}  // namespace

QString readScript(const QString &path, QString *error) {
    const auto failed = [error](const QString &why) {
        if (error) *error = why;
        return QString();
    };
    // A FIFO would never open and /dev/zero never end.
    const QFileInfo info(path);
    if (!info.exists()) return failed(said("The script is not there any more: %1").arg(path));
    if (!info.isFile()) return failed(said("That is not an ordinary file: %1").arg(path));
    if (info.size() > kMaxScriptBytes) return failed(said("That file is too big to be a script: %1").arg(path));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return failed(said("Could not read %1").arg(path));
    const QByteArray bytes = file.read(kMaxScriptBytes);

    // A word processor's text export may be UTF-16, marked as such at its start.
    const std::optional<QStringConverter::Encoding> marked = QStringConverter::encodingForData(bytes);
    // Stateless: this is all there is, so a character cut off at the end is an error,
    // not one waiting for the rest.
    QStringDecoder decoder(marked.value_or(QStringConverter::Utf8), QStringConverter::Flag::Stateless);
    QString text = decoder.decode(bytes);
    // Not UTF-8 and unmarked: an older text file, in Latin-1 as near as can be told.
    if (decoder.hasError()) text = QString::fromLatin1(bytes);
    if (text.contains(QChar(0))) return failed(said("That is not a text file: %1").arg(path));

    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QString suffix = info.suffix().toLower();
    if (suffix == QLatin1String("md") || suffix == QLatin1String("markdown")) {
        QTextDocument document;
        document.setMarkdown(text);
        text = document.toPlainText();
    }
    text = text.trimmed();
    if (text.isEmpty()) return failed(said("There are no words in %1").arg(path));
    if (error) error->clear();
    return text;
}

}  // namespace prompter
