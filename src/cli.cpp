#include "cli.h"

#include "documentstore.h"
#include "markdown.h"
#include "spellchecker.h"
#include "textmetrics.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cstdio>

namespace {

void emitLine(const QString &text, FILE *stream, QString *capture) {
    const QString line = text.endsWith(QLatin1Char('\n')) ? text : (text + QLatin1Char('\n'));
    if (capture)
        capture->append(line);
    else
        std::fputs(line.toUtf8().constData(), stream);
}

int runStats(const QStringList &args, QString *out, QString *err) {
    bool json = false;
    QString filePath;
    for (int i = 2; i < args.size(); ++i) {
        if (args.at(i) == QStringLiteral("--json"))
            json = true;
        else if (args.at(i) == QStringLiteral("--help") || args.at(i) == QStringLiteral("-h")) {
            emitLine(QStringLiteral("Usage: gonzo stats [--json] <file>"), stdout, out);
            return 0;
        } else if (args.at(i).startsWith(QLatin1Char('-'))) {
            emitLine(QStringLiteral("Unknown option: %1").arg(args.at(i)), stderr, err);
            return 1;
        } else {
            filePath = args.at(i);
        }
    }

    if (filePath.isEmpty()) {
        emitLine(QStringLiteral("Usage: gonzo stats [--json] <file>"), stderr, err);
        return 1;
    }

    const ReadFileResult read = DocumentStore::readFile(filePath);
    if (!read.ok) {
        if (json) {
            QJsonObject obj{{QStringLiteral("ok"), false}, {QStringLiteral("error"), read.error}};
            emitLine(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)), stdout, out);
        }
        emitLine(read.error, stderr, err);
        return 1;
    }

    const TextMetrics metrics(read.text);
    const QList<MarkdownSpan> spans = Markdown::spans(read.text);
    int headings = 0;
    int links = 0;
    for (const MarkdownSpan &s : spans) {
        if (s.style == MarkdownStyle::Heading)
            ++headings;
        else if (s.style == MarkdownStyle::Link)
            ++links;
    }

    int paragraphs = 0;
    bool inParagraph = false;
    for (const QString &line : EditorCommands::normalize(read.text).split(QLatin1Char('\n'))) {
        if (line.trimmed().isEmpty()) {
            inParagraph = false;
        } else if (!inParagraph) {
            ++paragraphs;
            inParagraph = true;
        }
    }

    if (json) {
        QJsonObject obj{
            {QStringLiteral("ok"), true},
            {QStringLiteral("path"), read.resolvedPath},
            {QStringLiteral("words"), metrics.wordCount()},
            {QStringLiteral("characters"), static_cast<qint64>(read.text.size())},
            {QStringLiteral("bytes"), read.utf8Bytes},
            {QStringLiteral("paragraphs"), paragraphs},
            {QStringLiteral("headings"), headings},
            {QStringLiteral("links"), links}
        };
        emitLine(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)), stdout, out);
    } else {
        emitLine(QStringLiteral("%1: %2 words, %3 paragraphs, %4 headings, %5 links, %6 bytes")
                     .arg(read.resolvedPath)
                     .arg(metrics.wordCount())
                     .arg(paragraphs)
                     .arg(headings)
                     .arg(links)
                     .arg(read.utf8Bytes),
                 stdout, out);
    }
    return 0;
}

int runCheck(const QStringList &args, QString *out, QString *err) {
    bool json = false;
    bool spell = false;
    QString filePath;
    for (int i = 2; i < args.size(); ++i) {
        if (args.at(i) == QStringLiteral("--json"))
            json = true;
        else if (args.at(i) == QStringLiteral("--spell"))
            spell = true;
        else if (args.at(i) == QStringLiteral("--help") || args.at(i) == QStringLiteral("-h")) {
            emitLine(QStringLiteral("Usage: gonzo check [--json] [--spell] <file>"), stdout, out);
            return 0;
        } else if (args.at(i).startsWith(QLatin1Char('-'))) {
            emitLine(QStringLiteral("Unknown option: %1").arg(args.at(i)), stderr, err);
            return 1;
        } else {
            filePath = args.at(i);
        }
    }

    if (filePath.isEmpty()) {
        emitLine(QStringLiteral("Usage: gonzo check [--json] [--spell] <file>"), stderr, err);
        return 1;
    }

    const ReadFileResult read = DocumentStore::readFile(filePath);
    if (!read.ok) {
        if (json) {
            QJsonObject obj{{QStringLiteral("ok"), false}, {QStringLiteral("error"), read.error}};
            emitLine(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)), stdout, out);
        }
        emitLine(read.error, stderr, err);
        return 1;
    }

    QJsonArray spellingIssues;
    bool spellChecked = false;
    if (spell) {
        SpellChecker checker;
        if (!checker.isAvailable()) {
            const QString msg = QStringLiteral("No Hunspell dictionary installed.");
            if (json) {
                QJsonObject obj{{QStringLiteral("ok"), false}, {QStringLiteral("error"), msg}};
                emitLine(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)), stdout, out);
            }
            emitLine(msg, stderr, err);
            return 1;
        }
        spellChecked = true;
        const QStringList lines = read.text.split(QLatin1Char('\n'));
        for (int lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
            const QString &line = lines.at(lineIdx);
            for (const TextRange &r : checker.misspelledRanges(line)) {
                spellingIssues.append(QJsonObject{
                    {QStringLiteral("line"), lineIdx + 1},
                    {QStringLiteral("column"), r.start + 1},
                    {QStringLiteral("word"), line.mid(r.start, r.length)}
                });
            }
        }
    }

    const bool ok = spellingIssues.isEmpty();
    if (json) {
        QJsonObject obj{
            {QStringLiteral("ok"), ok},
            {QStringLiteral("path"), read.resolvedPath},
            {QStringLiteral("bytes"), read.utf8Bytes},
            {QStringLiteral("spellChecked"), spellChecked},
            {QStringLiteral("spellingIssues"), spellingIssues}
        };
        emitLine(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)), stdout, out);
    } else if (ok) {
        emitLine(QStringLiteral("OK: %1 (%2 bytes)").arg(read.resolvedPath).arg(read.utf8Bytes), stdout, out);
    } else {
        for (const QJsonValue &val : spellingIssues) {
            const QJsonObject item = val.toObject();
            emitLine(QStringLiteral("%1:%2:%3: misspelled '%4'")
                         .arg(read.resolvedPath)
                         .arg(item.value(QStringLiteral("line")).toInt())
                         .arg(item.value(QStringLiteral("column")).toInt())
                         .arg(item.value(QStringLiteral("word")).toString()),
                     stderr, err);
        }
    }
    return ok ? 0 : 1;
}

int runSkill(const QStringList &args, QString *out, QString *err, const QString &homeOverride) {
    QFile bundled(QStringLiteral(":/skill.md"));
    if (!bundled.open(QIODevice::ReadOnly)) {
        emitLine(QStringLiteral("Bundled skill is missing."), stderr, err);
        return 1;
    }
    const QByteArray text = bundled.readAll();
    const QString sub = args.size() > 2 ? args.at(2) : QString();
    if (sub.isEmpty()) {
        emitLine(QString::fromUtf8(text), stdout, out);
        return 0;
    }
    if (sub != QStringLiteral("install")) {
        emitLine(QStringLiteral("Use `gonzo skill` to print the skill, or `gonzo skill install`."), stderr, err);
        return 1;
    }

    const QDir home(homeOverride.isEmpty() ? QDir::homePath() : homeOverride);
    const QString directory = home.filePath(QStringLiteral(".agents/skills/gonzo"));
    QSaveFile file(directory + QStringLiteral("/SKILL.md"));
    if (!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly)
        || file.write(text) != text.size() || !file.commit()) {
        emitLine(QStringLiteral("Could not write %1").arg(file.fileName()), stderr, err);
        return 1;
    }
    emitLine(QStringLiteral("Installed %1").arg(file.fileName()), stdout, out);

    if (!home.exists(QStringLiteral(".claude")))
        return 0;

    const QString link = home.filePath(QStringLiteral(".claude/skills/gonzo"));
    const QFileInfo existing(link);
    if (existing.isSymLink())
        QFile::remove(link);
    else if (existing.exists()) {
        emitLine(QStringLiteral("%1 exists and is not a link; leaving it alone.").arg(link), stderr, err);
        return 1;
    }
    if (!home.mkpath(QStringLiteral(".claude/skills"))
        || !QFile::link(QStringLiteral("../../.agents/skills/gonzo"), link)) {
        emitLine(QStringLiteral("Could not link %1").arg(link), stderr, err);
        return 1;
    }
    emitLine(QStringLiteral("Linked %1").arg(link), stdout, out);
    return 0;
}

} // namespace

namespace Cli {

bool isCliCommand(const QStringList &arguments) {
    if (arguments.size() < 2)
        return false;
    const QString &cmd = arguments.at(1);
    return cmd == QStringLiteral("-h")
        || cmd == QStringLiteral("--help")
        || cmd == QStringLiteral("help")
        || cmd == QStringLiteral("-v")
        || cmd == QStringLiteral("--version")
        || cmd == QStringLiteral("stats")
        || cmd == QStringLiteral("check")
        || cmd == QStringLiteral("skill");
}

QString usageText() {
    return QStringLiteral(
        "Usage: gonzo [file]\n"
        "       gonzo open [file]\n"
        "       gonzo stats [--json] <file>\n"
        "       gonzo check [--json] [--spell] <file>\n"
        "       gonzo skill [install]\n"
        "       gonzo --version | --help");
}

int run(const QStringList &arguments, QString *stdoutOut, QString *stderrOut,
        const QString &homeOverride) {
    const QString cmd = arguments.size() > 1 ? arguments.at(1) : QString();
    if (cmd == QStringLiteral("-h") || cmd == QStringLiteral("--help") || cmd == QStringLiteral("help")) {
        emitLine(usageText(), stdout, stdoutOut);
        return 0;
    }
    if (cmd == QStringLiteral("-v") || cmd == QStringLiteral("--version")) {
        emitLine(QStringLiteral("gonzo 0.1.0"), stdout, stdoutOut);
        return 0;
    }
    if (cmd == QStringLiteral("stats"))
        return runStats(arguments, stdoutOut, stderrOut);
    if (cmd == QStringLiteral("check"))
        return runCheck(arguments, stdoutOut, stderrOut);
    if (cmd == QStringLiteral("skill"))
        return runSkill(arguments, stdoutOut, stderrOut, homeOverride);

    emitLine(usageText(), stderr, stderrOut);
    return 1;
}

std::optional<QString> fileArgumentForGui(const QStringList &arguments) {
    if (arguments.size() < 2)
        return std::nullopt;
    if (arguments.at(1) == QStringLiteral("open")) {
        if (arguments.size() >= 3)
            return arguments.at(2);
        return std::nullopt;
    }
    return arguments.at(1);
}

} // namespace Cli
