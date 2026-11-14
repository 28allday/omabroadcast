#pragma once

#include <QString>
#include <QStringList>

// What the command line asks of a start: safe mode, a file to open, and,
// when restarting, a copy of the app to wait for first.
struct StartOptions {
    bool safeMode = false;
    qint64 waitFor = 0;  // a process id
    QString open;        // a file to open

    // arguments[0] is the program, as QCoreApplication::arguments() has it.
    static StartOptions parse(const QStringList &arguments) {
        StartOptions o;
        for (int i = 1; i < arguments.size(); ++i) {
            const QString &a = arguments.at(i);
            if (a == QLatin1String("--safe-mode")) o.safeMode = true;
            else if (a == QLatin1String("--wait-for") && i + 1 < arguments.size()) o.waitFor = arguments.at(++i).toLongLong();
            else if (!a.startsWith(QLatin1String("--")) && o.open.isEmpty()) o.open = a;
        }
        return o;
    }
    // A normal start once this copy (`self`) has gone. A file given before
    // is in the collection by then, so it is not opened again.
    static QStringList restartArguments(qint64 self) {
        return {QStringLiteral("--wait-for"), QString::number(self)};
    }
};
