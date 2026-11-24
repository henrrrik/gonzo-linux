#include "markdown.h"

#include <QRegularExpression>
#include <QUrl>
#include <algorithm>
#include <limits>

namespace {

struct LinkPart {
    TextRange whole;
    TextRange label;
    TextRange destination;
};

// Visit each UTF-16 code unit at most once so incomplete or deeply nested
// brackets/backslashes can never trigger regex backtracking.
QList<LinkPart> linkParts(const QString &text) {
    QList<LinkPart> result;
    const int length = text.size();
    std::optional<int> opening;
    int index = 0;

    while (index < length) {
        const ushort ch = text.at(index).unicode();
        if (ch == '\\') {
            index += std::min(2, length - index);
            continue;
        }
        if (ch == '[')
            opening = index;
        if (ch == ']' && opening.has_value()) {
            const int start = *opening;
            opening.reset();
            if (index > start + 1 && index + 1 < length && text.at(index + 1).unicode() == '(') {
                const int labelEnd = index;
                index += 2;
                const int destinationStart = index;
                while (index < length && text.at(index).unicode() != ')') {
                    index += text.at(index).unicode() == '\\' ? std::min(2, length - index) : 1;
                }
                if (index < length && index > destinationStart) {
                    result.append(LinkPart{
                        TextRange{start, index - start + 1},
                        TextRange{start + 1, labelEnd - start - 1},
                        TextRange{destinationStart, index - destinationStart}
                    });
                }
            }
        }
        ++index;
    }
    return result;
}

template <typename Fn>
void forEachParagraph(const QString &source, Fn &&body) {
    // Bound every regex invocation, even for a file with a single huge line.
    const int length = source.size();
    int start = 0;
    for (int end = 0; end <= length; ++end) {
        const bool atEnd = end == length;
        const bool atBreak = !atEnd && Paragraphs::isBreak(source.at(end));
        if (atEnd || atBreak) {
            if (end > start && end - start <= Markdown::kMaxParagraphLength)
                body(source.mid(start, end - start), start);
            start = end + 1;
        }
    }
}

struct Rule {
    QRegularExpression regex;
    QList<std::pair<int, MarkdownStyle>> groups;
};

QRegularExpression compileParagraphRegex(const QString &pattern) {
    // Paragraphs are split before matching, so anchors bind strictly to the
    // paragraph start/end (never after U+000B, U+000C, or U+0085).
    return QRegularExpression(pattern);
}

const QRegularExpression &codeRegex() {
    static const QRegularExpression re = compileParagraphRegex(QStringLiteral("`([^`]+)`"));
    return re;
}

const QList<Rule> &markdownRules() {
    static const QList<Rule> rules = {
        {compileParagraphRegex(QStringLiteral("^(#{1,6}[\\t ]+)(.*)$")),
         {{1, MarkdownStyle::Marker}, {2, MarkdownStyle::Heading}}},
        {compileParagraphRegex(QStringLiteral("^(\\s*>+\\s?)(.*)$")),
         {{1, MarkdownStyle::Marker}, {2, MarkdownStyle::Quote}}},
        {compileParagraphRegex(QStringLiteral("^(\\s*(?:[-+*]|\\d+[.)])\\s+)")),
         {{1, MarkdownStyle::Marker}}},
        {compileParagraphRegex(QStringLiteral("^\\s{0,3}([-*_])(?:\\s*\\1){2,}\\s*$")),
         {{0, MarkdownStyle::Marker}}},
        {compileParagraphRegex(QStringLiteral("(\\*\\*|__)(.+?)(\\1)")),
         {{1, MarkdownStyle::Marker}, {2, MarkdownStyle::Bold}, {3, MarkdownStyle::Marker}}},
        {compileParagraphRegex(QStringLiteral("(?<!\\*)\\*([^*\n]+)\\*(?!\\*)|(?<!_)_([^_\n]+)_(?!_)")),
         {{0, MarkdownStyle::Marker}, {1, MarkdownStyle::Italic}, {2, MarkdownStyle::Italic}}},
        // Code runs last so emphasis inside backticks is displayed as code.
        {codeRegex(), {{0, MarkdownStyle::Code}}}
    };
    return rules;
}

} // namespace

namespace Paragraphs {

bool isBreak(QChar ch) {
    const ushort u = ch.unicode();
    return u == 10 || u == 13 || u == 0x2028 || u == 0x2029;
}

TextRange range(const QString &text, TextRange covering) {
    const int length = text.size();
    int start = std::clamp(covering.start, 0, length);
    int end = std::clamp(covering.end(), start, length);

    // Treat CRLF as a single boundary even when the caret sits between CR and LF.
    if (start > 0 && start < length && text.at(start).unicode() == 10 && text.at(start - 1).unicode() == 13)
        --start;
    while (start > 0 && !isBreak(text.at(start - 1)))
        --start;

    if (end > 0 && covering.length > 0 && isBreak(text.at(end - 1))) {
        if (end < length && text.at(end - 1).unicode() == 13 && text.at(end).unicode() == 10)
            ++end;
    } else {
        while (end < length && !isBreak(text.at(end)))
            ++end;
        if (end < length) {
            const ushort delim = text.at(end).unicode();
            ++end;
            if (delim == 13 && end < length && text.at(end).unicode() == 10)
                ++end;
        }
    }
    return TextRange{start, end - start};
}

} // namespace Paragraphs

namespace Markdown {

QList<MarkdownSpan> paragraphSpans(const QString &paragraph) {
    QList<MarkdownSpan> result;
    if (paragraph.isEmpty() || paragraph.size() > kMaxParagraphLength)
        return result;

    for (const Rule &rule : markdownRules()) {
        const bool isCodeRule = rule.groups.size() == 1 && rule.groups.first().second == MarkdownStyle::Code;
        if (isCodeRule) {
            for (const LinkPart &link : linkParts(paragraph)) {
                result.append(MarkdownSpan{link.whole.start, link.whole.length, MarkdownStyle::Marker, 0});
                result.append(MarkdownSpan{link.label.start, link.label.length, MarkdownStyle::Link, 0});
            }
        }

        QRegularExpressionMatchIterator it = rule.regex.globalMatch(paragraph);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            for (const auto &[group, style] : rule.groups) {
                const int start = static_cast<int>(match.capturedStart(group));
                const int len = static_cast<int>(match.capturedLength(group));
                if (start < 0 || len <= 0)
                    continue;
                if (style == MarkdownStyle::Heading) {
                    const QString prefix = match.captured(1);
                    int level = 0;
                    while (level < prefix.size() && prefix.at(level) == QLatin1Char('#'))
                        ++level;
                    result.append(MarkdownSpan{start, len, MarkdownStyle::Heading, std::clamp(level, 1, 6)});
                } else {
                    result.append(MarkdownSpan{start, len, style, 0});
                }
            }
        }
    }
    return result;
}

QList<MarkdownSpan> spans(const QString &text) {
    QList<MarkdownSpan> result;
    forEachParagraph(text, [&result](const QString &paragraph, int start) {
        for (MarkdownSpan span : paragraphSpans(paragraph)) {
            span.start += start;
            result.append(span);
        }
    });
    return result;
}

QList<TextRange> paragraphSpellingExclusions(const QString &paragraph) {
    QList<TextRange> ranges;
    if (paragraph.isEmpty() || paragraph.size() > kMaxParagraphLength)
        return ranges;

    QRegularExpressionMatchIterator it = codeRegex().globalMatch(paragraph);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        ranges.append(TextRange{static_cast<int>(match.capturedStart(0)),
                                static_cast<int>(match.capturedLength(0))});
    }
    for (const LinkPart &link : linkParts(paragraph))
        ranges.append(link.destination);
    return ranges;
}

QList<TextRange> spellingExclusions(const QString &text) {
    QList<TextRange> ranges;
    forEachParagraph(text, [&ranges](const QString &paragraph, int start) {
        for (TextRange r : paragraphSpellingExclusions(paragraph)) {
            r.start += start;
            ranges.append(r);
        }
    });
    return ranges;
}

} // namespace Markdown

namespace EditorCommands {

TextMutation wrap(const QString &text, TextRange selection, const QString &marker) {
    const QString selected = text.mid(selection.start, selection.length);
    return TextMutation{
        selection,
        marker + selected + marker,
        TextRange{selection.start + static_cast<int>(marker.size()), selection.length}
    };
}

TextMutation link(const QString &text, TextRange selection, const std::optional<QString> &destination) {
    const QString label = selection.length == 0
        ? QStringLiteral("link text")
        : text.mid(selection.start, selection.length);
    QString escapedLabel = label;
    escapedLabel.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escapedLabel.replace(QLatin1Char('['), QStringLiteral("\\["));
    escapedLabel.replace(QLatin1Char(']'), QStringLiteral("\\]"));

    QString url = destination.value_or(QStringLiteral("https://"));
    url.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    url.replace(QLatin1Char('('), QStringLiteral("\\("));
    url.replace(QLatin1Char(')'), QStringLiteral("\\)"));

    const QString replacement = QLatin1Char('[') + escapedLabel + QStringLiteral("](") + url + QLatin1Char(')');
    const int labelLength = static_cast<int>(escapedLabel.size());

    TextRange newSelection;
    if (selection.length == 0) {
        newSelection = TextRange{selection.start + 1, labelLength};
    } else if (!destination.has_value()) {
        newSelection = TextRange{selection.start + labelLength + 3, static_cast<int>(url.size())};
    } else {
        newSelection = TextRange{selection.start + static_cast<int>(replacement.size()), 0};
    }
    return TextMutation{selection, replacement, newSelection};
}

std::optional<QString> linkDestination(const QString &text) {
    if (text.size() > 2048)
        return std::nullopt;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return std::nullopt;
    const QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid())
        return std::nullopt;
    const QString scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("https") && scheme != QStringLiteral("http")
        && scheme != QStringLiteral("mailto"))
        return std::nullopt;
    return trimmed;
}

TextMutation newline(const QString &text, TextRange selection, bool soft,
                     std::optional<TextRange> paragraphRange) {
    static const QRegularExpression listRegex(
        QStringLiteral("^(\\s*)([-+*]|\\d+[.)]|>+)\\s+(.*)$"));

    TextRange range = selection;
    QString insertion = soft ? QStringLiteral("\n") : QStringLiteral("\n\n");

    if (!soft) {
        const TextRange paragraph = paragraphRange.value_or(
            Paragraphs::range(text, TextRange{selection.start, 0}));
        const int prefixLen = std::min(Markdown::kMaxParagraphLength,
                                       std::max(0, selection.start - paragraph.start));
        const QString line = text.mid(paragraph.start, prefixLen);
        const QRegularExpressionMatch match = listRegex.match(line);
        if (match.hasMatch()) {
            // An item is empty only when nothing but whitespace follows the
            // caret too; Return right after a marker must keep the item text.
            const int restStart = selection.end();
            const int restLen = std::min(Markdown::kMaxParagraphLength,
                                         std::max(0, paragraph.end() - restStart));
            const QString rest = text.mid(restStart, restLen);
            const bool emptyAfter = rest.trimmed().isEmpty();

            if (match.capturedLength(3) == 0 && emptyAfter
                && selection.start - paragraph.start <= Markdown::kMaxParagraphLength) {
                range = TextRange{paragraph.start, selection.end() - paragraph.start};
                insertion = QStringLiteral("\n");
            } else {
                QString marker = match.captured(2);
                if (marker.size() >= 2 && (marker.endsWith(QLatin1Char('.')) || marker.endsWith(QLatin1Char(')')))) {
                    bool ok = false;
                    const qulonglong number = marker.left(marker.size() - 1).toULongLong(&ok);
                    if (ok && number < static_cast<qulonglong>(std::numeric_limits<int>::max())) {
                        marker = QString::number(number + 1) + marker.right(1);
                    }
                }
                insertion = QLatin1Char('\n') + match.captured(1) + marker + QLatin1Char(' ');
            }
        }
    }

    return TextMutation{
        range,
        insertion,
        TextRange{range.start + static_cast<int>(insertion.size()), 0}
    };
}

std::optional<TextMutation> deleteBackward(const QString &text, TextRange selection) {
    if (selection.length == 0 && selection.start >= 2
        && text.at(selection.start - 2) == QLatin1Char('\n')
        && text.at(selection.start - 1) == QLatin1Char('\n')) {
        const TextRange range{selection.start - 2, 2};
        return TextMutation{range, QString(), TextRange{range.start, 0}};
    }
    return std::nullopt;
}

QString normalize(const QString &text) {
    QString out = text;
    out.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    out.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return out;
}

} // namespace EditorCommands
