#include "highlighter.h"

#include "markdown.h"
#include "spellchecker.h"
#include "theme.h"

#include <QTextBlock>
#include <algorithm>

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document) {
    if (document)
        m_baseFont = document->defaultFont();
    rebuildFormats();
}

qreal MarkdownHighlighter::headingScale(int level) {
    static constexpr qreal kScales[6] = {1.75, 1.45, 1.25, 1.12, 1.06, 1.0};
    return kScales[std::clamp(level, 1, 6) - 1];
}

void MarkdownHighlighter::setTheme(const Theme *theme) {
    if (!theme)
        return;
    m_foreground = QColor(theme->foreground());
    m_muted = QColor(theme->muted());
    m_accent = QColor(theme->accent());
    m_accentForeground = QColor(theme->accentForeground());
    m_codeBackground = QColor(theme->codeBackground());
    m_linkColor = QColor(theme->linkColor());
    rebuildFormats();
    rehighlight();
}

void MarkdownHighlighter::setBaseFont(const QFont &font) {
    if (m_baseFont == font)
        return;
    m_baseFont = font;
    rebuildFormats();
    rehighlight();
}

void MarkdownHighlighter::setSpellChecker(const SpellChecker *checker, bool enabled) {
    m_spellChecker = checker;
    m_spellCheckEnabled = enabled;
    rehighlight();
}

void MarkdownHighlighter::setSearch(const QString &query, int currentMatchStart) {
    if (m_searchQuery == query && m_currentMatchStart == currentMatchStart)
        return;
    m_searchQuery = query;
    m_currentMatchStart = currentMatchStart;
    rehighlight();
}

void MarkdownHighlighter::rebuildFormats() {
    m_markerFormat = QTextCharFormat();
    m_markerFormat.setForeground(m_muted);

    m_headingBaseFormat = QTextCharFormat();
    m_headingBaseFormat.setForeground(m_foreground);

    m_boldFormat = QTextCharFormat();
    m_boldFormat.setFontWeight(QFont::Bold);
    m_boldFormat.setForeground(m_foreground);

    m_italicFormat = QTextCharFormat();
    m_italicFormat.setFontItalic(true);
    m_italicFormat.setForeground(m_foreground);

    m_quoteFormat = QTextCharFormat();
    m_quoteFormat.setForeground(m_muted);
    m_quoteFormat.setFontItalic(true);

    m_codeFormat = QTextCharFormat();
    m_codeFormat.setFont(m_baseFont);
    m_codeFormat.setForeground(m_foreground);
    m_codeFormat.setBackground(m_codeBackground);

    m_linkFormat = QTextCharFormat();
    m_linkFormat.setForeground(m_linkColor);
    m_linkFormat.setFontUnderline(true);

    m_spellErrorFormat = QTextCharFormat();
    m_spellErrorFormat.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    m_spellErrorFormat.setUnderlineColor(QColor(QStringLiteral("#D94A38")));

    QColor matchBg = m_accent;
    matchBg.setAlpha(72);
    m_searchFormat = QTextCharFormat();
    m_searchFormat.setBackground(matchBg);

    m_currentSearchFormat = QTextCharFormat();
    m_currentSearchFormat.setBackground(m_accent);
    m_currentSearchFormat.setForeground(m_accentForeground);
}

void MarkdownHighlighter::mergeFormat(int start, int length, const QTextCharFormat &overlay) {
    if (length <= 0)
        return;
    const int end = start + length;
    int runStart = start;
    QTextCharFormat runBase = format(runStart);

    for (int i = start + 1; i <= end; ++i) {
        const bool same = (i < end) && (format(i) == runBase);
        if (!same) {
            QTextCharFormat merged = runBase;
            merged.merge(overlay);
            setFormat(runStart, i - runStart, merged);
            if (i < end) {
                runStart = i;
                runBase = format(i);
            }
        }
    }
}

void MarkdownHighlighter::highlightBlock(const QString &text) {
    // 1. Markdown paragraph styling (bounded at 4,096 UTF-16 code units).
    if (!text.isEmpty() && text.size() <= Markdown::kMaxParagraphLength) {
        const QList<MarkdownSpan> spans = Markdown::paragraphSpans(text);
        const qreal basePt = m_baseFont.pointSizeF() > 0.0 ? m_baseFont.pointSizeF() : 13.0;

        for (const MarkdownSpan &span : spans) {
            switch (span.style) {
            case MarkdownStyle::Heading: {
                QTextCharFormat headingBlockFmt;
                headingBlockFmt.setFontWeight(QFont::Bold);
                headingBlockFmt.setFontPointSize(basePt * headingScale(span.headingLevel));
                mergeFormat(0, static_cast<int>(text.size()), headingBlockFmt);
                mergeFormat(span.start, span.length, m_headingBaseFormat);
                break;
            }
            case MarkdownStyle::Marker:
                mergeFormat(span.start, span.length, m_markerFormat);
                break;
            case MarkdownStyle::Bold:
                mergeFormat(span.start, span.length, m_boldFormat);
                break;
            case MarkdownStyle::Italic:
                mergeFormat(span.start, span.length, m_italicFormat);
                break;
            case MarkdownStyle::Quote:
                mergeFormat(span.start, span.length, m_quoteFormat);
                break;
            case MarkdownStyle::Link:
                mergeFormat(span.start, span.length, m_linkFormat);
                break;
            case MarkdownStyle::Code:
                // Code runs last and resets font/weight so emphasis inside
                // backticks displays as plain monospace code.
                setFormat(span.start, span.length, m_codeFormat);
                break;
            }
        }
    }

    // 2. Optional Hunspell spell checking (skips inline code and link URLs).
    if (m_spellChecker && m_spellCheckEnabled && m_spellChecker->isAvailable()) {
        const QList<TextRange> misspelled = m_spellChecker->misspelledRanges(text);
        for (const TextRange &r : misspelled)
            mergeFormat(r.start, r.length, m_spellErrorFormat);
    }

    // 3. Incremental search match highlights.
    if (!m_searchQuery.isEmpty()) {
        const int blockPos = currentBlock().position();
        const int qLen = static_cast<int>(m_searchQuery.size());
        qsizetype idx = 0;
        while ((idx = text.indexOf(m_searchQuery, idx, Qt::CaseInsensitive)) >= 0) {
            const int globalStart = blockPos + static_cast<int>(idx);
            const QTextCharFormat &fmt = (globalStart == m_currentMatchStart)
                ? m_currentSearchFormat
                : m_searchFormat;
            mergeFormat(static_cast<int>(idx), qLen, fmt);
            idx += std::max(1, qLen);
        }
    }
}
