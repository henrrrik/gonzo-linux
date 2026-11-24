#pragma once

#include <QFont>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

class SpellChecker;
class Theme;

// Paragraph-local Markdown syntax highlighter keeping markers visible and
// dimmed while scaling H1-H6 headings and overlaying search & spellcheck cues.
class MarkdownHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit MarkdownHighlighter(QTextDocument *document);

    void setTheme(const Theme *theme);
    void setBaseFont(const QFont &font);
    void setSpellChecker(const SpellChecker *checker, bool enabled);
    void setSearch(const QString &query, int currentMatchStart);

    int highlightBlockCount() const { return m_highlightBlockCount; }
    void resetHighlightBlockCount() { m_highlightBlockCount = 0; }

    static qreal headingScale(int level);

protected:
    void highlightBlock(const QString &text) override;

private:
    void mergeFormat(int start, int length, const QTextCharFormat &overlay);
    void rebuildFormats();

    QFont m_baseFont;
    QColor m_foreground = QColor(QStringLiteral("#e8e8ec"));
    QColor m_muted = QColor(QStringLiteral("#8a8a94"));
    QColor m_accent = QColor(QStringLiteral("#FFD60A"));
    QColor m_accentForeground = QColor(QStringLiteral("#000000"));
    QColor m_codeBackground = QColor(QStringLiteral("#1c1c22"));
    QColor m_linkColor = QColor(QStringLiteral("#FFD60A"));

    QTextCharFormat m_markerFormat;
    QTextCharFormat m_headingBaseFormat;
    QTextCharFormat m_boldFormat;
    QTextCharFormat m_italicFormat;
    QTextCharFormat m_quoteFormat;
    QTextCharFormat m_codeFormat;
    QTextCharFormat m_linkFormat;
    QTextCharFormat m_spellErrorFormat;
    QTextCharFormat m_searchFormat;
    QTextCharFormat m_currentSearchFormat;

    const SpellChecker *m_spellChecker = nullptr;
    bool m_spellCheckEnabled = false;
    QString m_searchQuery;
    int m_currentMatchStart = -1;
    int m_highlightBlockCount = 0;
};
