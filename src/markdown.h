#pragma once

#include <QList>
#include <QString>
#include <optional>
#include <utility>

enum class MarkdownStyle {
    Heading,
    Marker,
    Bold,
    Italic,
    Code,
    Quote,
    Link
};

struct MarkdownSpan {
    int start = 0;
    int length = 0;
    MarkdownStyle style = MarkdownStyle::Marker;
    int headingLevel = 0;

    bool operator==(const MarkdownSpan &other) const {
        return start == other.start && length == other.length
            && style == other.style && headingLevel == other.headingLevel;
    }
};

struct TextRange {
    int start = 0;
    int length = 0;

    int end() const { return start + length; }
    bool operator==(const TextRange &other) const {
        return start == other.start && length == other.length;
    }
};

struct TextMutation {
    TextRange range;
    QString replacement;
    TextRange selection;

    bool operator==(const TextMutation &other) const {
        return range == other.range && replacement == other.replacement
            && selection == other.selection;
    }
};

// Paragraph-local Markdown scanner ported from henrrrik/gonzo-mac.
// Markers stay visible so source, selection, accessibility, and saved text agree.
namespace Markdown {
constexpr int kMaxParagraphLength = 4096;

QList<MarkdownSpan> spans(const QString &text);
QList<MarkdownSpan> paragraphSpans(const QString &paragraph);
QList<TextRange> spellingExclusions(const QString &text);
QList<TextRange> paragraphSpellingExclusions(const QString &paragraph);
} // namespace Markdown

namespace Paragraphs {
bool isBreak(QChar ch);
TextRange range(const QString &text, TextRange covering);
} // namespace Paragraphs

namespace EditorCommands {
TextMutation wrap(const QString &text, TextRange selection, const QString &marker);
TextMutation link(const QString &text, TextRange selection, const std::optional<QString> &destination);
std::optional<QString> linkDestination(const QString &text);
TextMutation newline(const QString &text, TextRange selection, bool soft,
                     std::optional<TextRange> paragraphRange = std::nullopt);
std::optional<TextMutation> deleteBackward(const QString &text, TextRange selection);
QString normalize(const QString &text);
} // namespace EditorCommands
