#pragma once

#include <QString>
#include <QStringList>
#include <optional>

// Headless CLI commands for scripts and agents:
//   gonzo --help | --version
//   gonzo open [file]
//   gonzo stats [--json] <file>
//   gonzo check [--json] [--spell] <file>
//   gonzo skill [install]
namespace Cli {
bool isCliCommand(const QStringList &arguments);
QString usageText();
int run(const QStringList &arguments, QString *stdoutOut = nullptr, QString *stderrOut = nullptr,
        const QString &homeOverride = {});
std::optional<QString> fileArgumentForGui(const QStringList &arguments);
} // namespace Cli
