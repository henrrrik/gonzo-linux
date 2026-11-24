// Omarchy live translator pattern from omarchy-app templates (MIT, David Heinemeier Hansson).
#include "translator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>

Translator::Translator(const QString &languageFile, const QString &catalogDirectory, QObject *parent)
    : QTranslator(parent),
      m_languageFile(languageFile.isEmpty() ? QDir::homePath() + QStringLiteral("/.config/omarchy/language") : languageFile),
      m_catalogDirectory(catalogDirectory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Translator::reload);
    reload();
}

QString Translator::readLanguage(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QStringLiteral("en");
    const QString code = QString::fromUtf8(file.readLine()).trimmed();
    static const QRegularExpression valid(QStringLiteral("^[a-z]{2,3}(-[A-Za-z]{2})?$"));
    return valid.match(code).hasMatch() ? code : QStringLiteral("en");
}

QString Translator::pluralCategory(const QString &language, long long count) {
    const long long n = count < 0 ? -count : count;
    const long long n10 = n % 10;
    const long long n100 = n % 100;
    const bool millions = n != 0 && n % 1000000 == 0;
    const bool teens = n100 >= 12 && n100 <= 14;
    const QString &code = language;
    auto in = [&](std::initializer_list<const char *> codes) {
        for (const char *c : codes) {
            if (code == QLatin1String(c))
                return true;
        }
        return false;
    };
    if (in({"ja", "ko", "zh-CN", "zh-TW", "th", "vi", "id", "ms"})) return QStringLiteral("other");
    if (in({"hi", "bn", "fa", "si"})) return n <= 1 ? QStringLiteral("one") : QStringLiteral("other");
    if (in({"fr", "pt-BR"})) return n <= 1 ? QStringLiteral("one") : millions ? QStringLiteral("many") : QStringLiteral("other");
    if (in({"ca", "es-MX", "it", "pt"})) return n == 1 ? QStringLiteral("one") : millions ? QStringLiteral("many") : QStringLiteral("other");
    if (code == QStringLiteral("fil")) return (n10 == 4 || n10 == 6 || n10 == 9) ? QStringLiteral("other") : QStringLiteral("one");
    if (code == QStringLiteral("is")) return n10 == 1 && n100 != 11 ? QStringLiteral("one") : QStringLiteral("other");
    if (in({"cs", "sk"})) return n == 1 ? QStringLiteral("one") : (n >= 2 && n <= 4) ? QStringLiteral("few") : QStringLiteral("other");
    if (code == QStringLiteral("pl")) return n == 1 ? QStringLiteral("one") : (n10 >= 2 && n10 <= 4 && !teens) ? QStringLiteral("few") : QStringLiteral("many");
    if (in({"ru", "uk"})) return (n10 == 1 && n100 != 11) ? QStringLiteral("one") : (n10 >= 2 && n10 <= 4 && !teens) ? QStringLiteral("few") : QStringLiteral("many");
    if (in({"hr", "sr"})) return (n10 == 1 && n100 != 11) ? QStringLiteral("one") : (n10 >= 2 && n10 <= 4 && !teens) ? QStringLiteral("few") : QStringLiteral("other");
    if (code == QStringLiteral("sl")) return n100 == 1 ? QStringLiteral("one") : n100 == 2 ? QStringLiteral("two") : (n100 >= 3 && n100 <= 4) ? QStringLiteral("few") : QStringLiteral("other");
    if (code == QStringLiteral("lt")) return (n100 >= 11 && n100 <= 19) ? QStringLiteral("other") : n10 == 1 ? QStringLiteral("one") : n10 >= 2 ? QStringLiteral("few") : QStringLiteral("other");
    if (code == QStringLiteral("lv")) return (n10 == 0 || (n100 >= 11 && n100 <= 19)) ? QStringLiteral("zero") : n10 == 1 ? QStringLiteral("one") : QStringLiteral("other");
    if (code == QStringLiteral("ro")) return n == 1 ? QStringLiteral("one") : (n == 0 || (n100 >= 1 && n100 <= 19)) ? QStringLiteral("few") : QStringLiteral("other");
    if (code == QStringLiteral("ar")) {
        if (n <= 2) return n == 0 ? QStringLiteral("zero") : n == 1 ? QStringLiteral("one") : QStringLiteral("two");
        return (n100 >= 3 && n100 <= 10) ? QStringLiteral("few") : n100 >= 11 ? QStringLiteral("many") : QStringLiteral("other");
    }
    if (code == QStringLiteral("he")) return n == 1 ? QStringLiteral("one") : n == 2 ? QStringLiteral("two") : QStringLiteral("other");
    if (code == QStringLiteral("ga")) return n == 1 ? QStringLiteral("one") : n == 2 ? QStringLiteral("two") : (n >= 3 && n <= 6) ? QStringLiteral("few") : (n >= 7 && n <= 10) ? QStringLiteral("many") : QStringLiteral("other");
    return n == 1 ? QStringLiteral("one") : QStringLiteral("other");
}

QLocale Translator::formatLocale(const QString &language) {
    const QString omarchyPath = qEnvironmentVariable("OMARCHY_PATH");
    if (!omarchyPath.isEmpty()) {
        QFile registry(omarchyPath + QStringLiteral("/default/i18n/locales.json"));
        if (registry.open(QIODevice::ReadOnly)) {
            const QString name = QJsonDocument::fromJson(registry.readAll()).object()
                                     .value(language).toObject().value(QStringLiteral("formatLocale")).toString();
            if (!name.isEmpty())
                return QLocale(name);
        }
    }
    return QLocale(language);
}

QString Translator::translate(const char *, const char *sourceText, const char *, int n) const {
    const QJsonValue value = m_strings.value(QString::fromUtf8(sourceText));
    if (value.isString())
        return value.toString();
    if (value.isObject()) {
        const QJsonObject forms = value.toObject();
        const QString form = forms.value(n < 0 ? QStringLiteral("other") : pluralCategory(m_language, n)).toString();
        return form.isEmpty() ? forms.value(QStringLiteral("other")).toString() : form;
    }
    return {};
}

void Translator::reload() {
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    const QString directory = QFileInfo(m_languageFile).absolutePath();
    if (QFileInfo::exists(directory))
        m_watcher.addPath(directory);
    if (QFileInfo::exists(m_languageFile))
        m_watcher.addPath(m_languageFile);

    const QString language = readLanguage(m_languageFile);
    if (language == m_language)
        return;

    QHash<QString, QJsonValue> strings;
    QStringList codes{QStringLiteral("en"), language.section(QLatin1Char('-'), 0, 0), language};
    codes.removeDuplicates();
    for (const auto &code : codes) {
        QFile catalog(m_catalogDirectory + QLatin1Char('/') + code + QStringLiteral(".json"));
        if (!catalog.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject object = QJsonDocument::fromJson(catalog.readAll()).object();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if ((it.value().isString() && !it.value().toString().isEmpty()) || it.value().isObject())
                strings.insert(it.key(), it.value());
        }
    }

    m_language = language;
    m_strings = strings;
    const QLocale locale = formatLocale(language);
    QLocale::setDefault(locale);
    if (qApp)
        QGuiApplication::setLayoutDirection(locale.textDirection());
    emit changed();
}
