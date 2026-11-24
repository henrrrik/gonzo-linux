#include "spellchecker.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QStringEncoder>
#include <hunspell/hunspell.hxx>

namespace {

bool rangeIntersectsAny(const TextRange &candidate, const QList<TextRange> &exclusions) {
    for (const TextRange &ex : exclusions) {
        if (candidate.start < ex.end() && ex.start < candidate.end())
            return true;
    }
    return false;
}

QStringList candidateDictionaryNames(const QString &languageCode) {
    QStringList names;
    auto add = [&names](const QString &code) {
        if (code.isEmpty())
            return;
        QString normalized = code;
        normalized.replace(QLatin1Char('-'), QLatin1Char('_'));
        if (!names.contains(normalized))
            names.append(normalized);
    };

    if (!languageCode.isEmpty()) {
        add(languageCode);
        const QLocale loc(languageCode);
        add(loc.name());
        add(languageCode.section(QLatin1Char('-'), 0, 0));
    }
    add(QLocale::system().name());
    add(QStringLiteral("en_US"));
    add(QStringLiteral("en_GB"));
    add(QStringLiteral("en"));
    return names;
}

} // namespace

SpellChecker::SpellChecker(const QString &languageCode, const QStringList &searchDirs)
    : m_searchDirs(searchDirs.isEmpty() ? defaultSearchDirs() : searchDirs) {
    loadDictionary(languageCode);
}

SpellChecker::~SpellChecker() = default;

QStringList SpellChecker::defaultSearchDirs() {
    return {
        QDir::homePath() + QStringLiteral("/.local/share/hunspell"),
        QStringLiteral("/usr/share/hunspell"),
        QStringLiteral("/usr/share/myspell/dicts"),
        QStringLiteral("/usr/share/myspell")
    };
}

void SpellChecker::setLanguage(const QString &languageCode) {
    loadDictionary(languageCode);
}

void SpellChecker::loadDictionary(const QString &languageCode) {
    m_hunspell.reset();
    m_dictionaryName.clear();
    m_dictEncoding = "UTF-8";

    auto initHunspell = [this](const QString &aff, const QString &dic, const QString &name) {
        m_hunspell = std::make_unique<Hunspell>(
            QFile::encodeName(aff).constData(),
            QFile::encodeName(dic).constData());
        m_dictionaryName = name;
        const std::string enc = m_hunspell->get_dict_encoding();
        m_dictEncoding = !enc.empty() ? QByteArray(enc.data(), static_cast<qsizetype>(enc.size())) : QByteArray("UTF-8");
    };

    const QStringList candidates = candidateDictionaryNames(languageCode);
    for (const QString &name : candidates) {
        for (const QString &dirPath : m_searchDirs) {
            const QString aff = dirPath + QLatin1Char('/') + name + QStringLiteral(".aff");
            const QString dic = dirPath + QLatin1Char('/') + name + QStringLiteral(".dic");
            if (QFileInfo::exists(aff) && QFileInfo::exists(dic)) {
                initHunspell(aff, dic, name);
                return;
            }
        }
    }

    // Fallback: pick any installed dictionary if none matched the requested locale.
    for (const QString &dirPath : m_searchDirs) {
        const QDir dir(dirPath);
        if (!dir.exists())
            continue;
        const QStringList affs = dir.entryList({QStringLiteral("*.aff")}, QDir::Files, QDir::Name);
        for (const QString &affFile : affs) {
            const QString base = affFile.left(affFile.size() - 4);
            const QString aff = dir.filePath(affFile);
            const QString dic = dir.filePath(base + QStringLiteral(".dic"));
            if (QFileInfo::exists(dic)) {
                initHunspell(aff, dic, base);
                return;
            }
        }
    }
}

std::string SpellChecker::encodeWord(const QString &word) const {
    if (m_dictEncoding.compare("UTF-8", Qt::CaseInsensitive) == 0)
        return word.toStdString();
    QStringEncoder encoder(m_dictEncoding.constData());
    if (!encoder.isValid())
        return word.toStdString();
    const QByteArray bytes = encoder.encode(word);
    return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}

QString SpellChecker::decodeWord(const std::string &bytes) const {
    if (m_dictEncoding.compare("UTF-8", Qt::CaseInsensitive) == 0)
        return QString::fromUtf8(bytes.c_str(), static_cast<qsizetype>(bytes.size()));
    QStringDecoder decoder(m_dictEncoding.constData());
    if (!decoder.isValid())
        return QString::fromUtf8(bytes.c_str(), static_cast<qsizetype>(bytes.size()));
    return decoder.decode(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

bool SpellChecker::isMisspelled(const QString &word) const {
    if (!m_hunspell || word.size() < 2 || m_ignored.contains(word))
        return false;
    // Skip purely numeric or mixed-digit tokens like "2026" or "16MB".
    for (const QChar ch : word) {
        if (ch.isDigit())
            return false;
    }
    return !m_hunspell->spell(encodeWord(word));
}

QStringList SpellChecker::suggestions(const QString &word, int maxSuggestions) const {
    if (!m_hunspell || word.isEmpty())
        return {};
    const std::vector<std::string> raw = m_hunspell->suggest(encodeWord(word));
    QStringList out;
    out.reserve(std::min<int>(static_cast<int>(raw.size()), maxSuggestions));
    for (const std::string &s : raw) {
        out.append(decodeWord(s));
        if (out.size() >= maxSuggestions)
            break;
    }
    return out;
}

void SpellChecker::ignoreWord(const QString &word) {
    if (word.isEmpty())
        return;
    m_ignored.insert(word);
    if (m_hunspell)
        m_hunspell->add(encodeWord(word));
}

QList<TextRange> SpellChecker::misspelledRanges(const QString &paragraph) const {
    QList<TextRange> results;
    if (!m_hunspell || paragraph.isEmpty() || paragraph.size() > Markdown::kMaxParagraphLength)
        return results;

    const QList<TextRange> exclusions = Markdown::paragraphSpellingExclusions(paragraph);
    static const QRegularExpression wordRe(QStringLiteral("\\p{L}+(?:['\\x{2019}]\\p{L}+)*"));
    QRegularExpressionMatchIterator it = wordRe.globalMatch(paragraph);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const TextRange range{static_cast<int>(m.capturedStart(0)),
                              static_cast<int>(m.capturedLength(0))};
        if (rangeIntersectsAny(range, exclusions))
            continue;
        if (isMisspelled(m.captured(0)))
            results.append(range);
    }
    return results;
}
