#pragma once

#include <QString>

// The words the prompter shows, from a text or Markdown file. Markdown's marks are
// taken out, as nobody reads a # aloud.
namespace prompter {

// Past this it is not a script, and a window would struggle to lay it out.
constexpr qint64 kMaxScriptBytes = 4 * 1024 * 1024;

// The text, or an empty string with `error` saying why.
QString readScript(const QString &path, QString *error);

}  // namespace prompter
