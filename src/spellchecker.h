#pragma once

#include "markdown.h"

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <memory>

class Hunspell;

// Optional Hunspell-backed spell checker that skips Markdown inline code spans
// and link destinations. Gracefully reports unavailable when no dictionaries
// are installed on the machine.
class SpellChecker {
public:
    explicit SpellChecker(const QString &languageCode = {},
                          const QStringList &searchDirs = {});
    ~SpellChecker();

    bool isAvailable() const { return m_hunspell != nullptr; }
    QString dictionaryName() const { return m_dictionaryName; }

    void setLanguage(const QString &languageCode);
    bool isMisspelled(const QString &word) const;
    QStringList suggestions(const QString &word, int maxSuggestions = 6) const;
    void ignoreWord(const QString &word);

    // Returns UTF-16 ranges of misspelled words in a single paragraph,
    // automatically excluding inline code spans and link URLs.
    QList<TextRange> misspelledRanges(const QString &paragraph) const;

    static QStringList defaultSearchDirs();

private:
    void loadDictionary(const QString &languageCode);
    std::string encodeWord(const QString &word) const;
    QString decodeWord(const std::string &bytes) const;

    QStringList m_searchDirs;
    QString m_dictionaryName;
    QByteArray m_dictEncoding;
    std::unique_ptr<Hunspell> m_hunspell;
    QSet<QString> m_ignored;
};
