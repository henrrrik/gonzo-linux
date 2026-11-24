// Theme watching follows Omacut and Omawrite (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QColor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <cmath>

namespace {

double relativeLuminance(const QColor &c) {
    auto linear = [](double v) {
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(c.redF()) + 0.7152 * linear(c.greenF()) + 0.0722 * linear(c.blueF());
}

QString blendHex(const QString &aHex, const QString &bHex, qreal factor) {
    const QColor a(aHex);
    const QColor b(bHex);
    if (!a.isValid() || !b.isValid())
        return aHex;
    const QColor mixed = QColor::fromRgbF(
        a.redF() + (b.redF() - a.redF()) * factor,
        a.greenF() + (b.greenF() - a.greenF()) * factor,
        a.blueF() + (b.blueF() - a.blueF()) * factor);
    return mixed.name(QColor::HexRgb);
}

QHash<QString, QString> parseColorsToml(const QString &path) {
    QHash<QString, QString> values;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return values;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        QString val = line.mid(eq + 1).trimmed();
        if (const int comment = val.indexOf(QStringLiteral(" #")); comment >= 0)
            val = val.left(comment).trimmed();
        if (val.size() >= 2 && ((val.front() == QLatin1Char('"') && val.back() == QLatin1Char('"'))
                                || (val.front() == QLatin1Char('\'') && val.back() == QLatin1Char('\'')))) {
            val = val.mid(1, val.size() - 2);
        }
        values.insert(key, val);
    }
    return values;
}

QString validHex(const QHash<QString, QString> &map, const QString &key) {
    const QString val = map.value(key);
    static const QRegularExpression hexRe(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return hexRe.match(val).hasMatch() ? val : QString();
}

} // namespace

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/state/omarchy/current") : directory),
      m_usePortal(directory.isEmpty()) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    if (m_usePortal) {
        bool known = false;
        const bool portalDark = detectPortalDarkMode(&known);
        if (known)
            m_portalDarkMode = portalDark;
        QDBusConnection::sessionBus().connect(
            QStringLiteral("org.freedesktop.portal.Desktop"),
            QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.Settings"),
            QStringLiteral("SettingChanged"),
            this,
            SLOT(handlePortalSettingChanged(QString,QString,QDBusVariant)));
    }

    // Corners follow Hyprland's effective rounding, which themes and personal
    // overrides can both set, so ask hyprctl rather than reading its config.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                  ? QStandardPaths::findExecutable(QStringLiteral("hyprctl")) : QString();
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this] { m_roundingTimeout.stop(); });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value(QStringLiteral("int"));
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    m_roundingPoll.setInterval(1000);
    connect(&m_roundingPoll, &QTimer::timeout, this, &Theme::refreshRounding);
    if (!m_hyprctl.isEmpty())
        m_roundingPoll.start();
    reload();
}

Theme::~Theme() {
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty() || m_roundingProcess.state() != QProcess::NotRunning)
        return;
    m_roundingProcess.start(m_hyprctl, {QStringLiteral("-j"), QStringLiteral("getoption"), QStringLiteral("decoration:rounding")});
    m_roundingTimeout.start();
}

QString Theme::readAccent(const QString &path) {
    const QHash<QString, QString> colors = parseColorsToml(path);
    const QString accent = validHex(colors, QStringLiteral("accent"));
    return accent.isEmpty() ? QStringLiteral("#FFD60A") : accent;
}

QString Theme::readableForeground(const QString &bgHex) {
    const QColor c(bgHex);
    if (!c.isValid())
        return QStringLiteral("white");
    return relativeLuminance(c) > 0.179 ? QStringLiteral("black") : QStringLiteral("white");
}

QString Theme::accentForeground() const {
    return readableForeground(m_accent);
}

QString Theme::selectionForeground() const {
    return readableForeground(m_selection);
}

bool Theme::detectPortalDarkMode(bool *known) const {
    if (known)
        *known = false;
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Settings"),
        QStringLiteral("Read"));
    call << QStringLiteral("org.freedesktop.appearance") << QStringLiteral("color-scheme");
    const QDBusReply<QDBusVariant> reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 150);
    if (!reply.isValid())
        return true;
    const QVariant inner = reply.value().variant();
    const uint scheme = inner.canConvert<QDBusVariant>()
        ? inner.value<QDBusVariant>().variant().toUInt()
        : inner.toUInt();
    if (scheme == 1 || scheme == 2) {
        if (known)
            *known = true;
        return scheme == 1;
    }
    return true;
}

void Theme::handlePortalSettingChanged(const QString &nameSpace, const QString &key,
                                       const QDBusVariant &value) {
    if (nameSpace != QStringLiteral("org.freedesktop.appearance") || key != QStringLiteral("color-scheme"))
        return;
    const uint scheme = value.variant().toUInt();
    if (scheme == 1 || scheme == 2) {
        m_portalDarkMode = (scheme == 1);
        reload();
    }
}

void Theme::reload() {
    refreshRounding();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and
    // re-arm after every change.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != QStringLiteral("/"))
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{
        ancestor,
        QFileInfo(ancestor).absolutePath(),
        m_directory,
        m_directory + QStringLiteral("/theme"),
        m_directory + QStringLiteral("/theme/colors.toml")
    };
    candidates.removeDuplicates();
    for (const auto &path : candidates) {
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);
    }

    const QHash<QString, QString> colors = parseColorsToml(m_directory + QStringLiteral("/theme/colors.toml"));

    bool dark = m_portalDarkMode;
    const QString mode = colors.value(QStringLiteral("mode")).toLower();
    QString bg = validHex(colors, QStringLiteral("background"));
    if (mode == QStringLiteral("light"))
        dark = false;
    else if (mode == QStringLiteral("dark"))
        dark = true;
    else if (!bg.isEmpty())
        dark = relativeLuminance(QColor(bg)) < 0.25;

    if (bg.isEmpty())
        bg = dark ? QStringLiteral("#0e0e10") : QStringLiteral("#fafafa");

    QString fg = validHex(colors, QStringLiteral("foreground"));
    if (fg.isEmpty())
        fg = dark ? QStringLiteral("#e8e8ec") : QStringLiteral("#18181b");

    QString accent = validHex(colors, QStringLiteral("accent"));
    if (accent.isEmpty())
        accent = QStringLiteral("#FFD60A");

    QString muted = validHex(colors, QStringLiteral("muted"));
    if (muted.isEmpty())
        muted = blendHex(fg, bg, 0.48);

    QString panelBg = validHex(colors, QStringLiteral("lighter_background"));
    if (panelBg.isEmpty() || panelBg == bg)
        panelBg = blendHex(bg, fg, dark ? 0.06 : 0.04);

    QString selection = validHex(colors, QStringLiteral("selection"));
    if (selection.isEmpty())
        selection = validHex(colors, QStringLiteral("selection_background"));
    if (selection.isEmpty() || selection == bg)
        selection = blendHex(bg, accent, dark ? 0.30 : 0.22);

    QString codeBg = validHex(colors, QStringLiteral("lighter_background"));
    if (codeBg.isEmpty() || codeBg == bg)
        codeBg = blendHex(bg, fg, dark ? 0.09 : 0.06);

    QString link = validHex(colors, QStringLiteral("blue"));
    if (link.isEmpty())
        link = accent;
    // Ensure links remain legible against the canvas even in monochrome themes.
    const double bgLum = relativeLuminance(QColor(bg));
    const double linkLum = relativeLuminance(QColor(link));
    const double contrast = (std::max(bgLum, linkLum) + 0.05) / (std::min(bgLum, linkLum) + 0.05);
    if (contrast < 3.0)
        link = accent;

    const QString border = blendHex(bg, fg, dark ? 0.14 : 0.12);

    const bool changedAny = dark != m_darkMode
        || bg != m_background
        || panelBg != m_panelBackground
        || fg != m_foreground
        || muted != m_muted
        || accent != m_accent
        || selection != m_selection
        || codeBg != m_codeBackground
        || link != m_linkColor
        || border != m_borderColor;

    m_darkMode = dark;
    m_background = bg;
    m_panelBackground = panelBg;
    m_foreground = fg;
    m_muted = muted;
    m_accent = accent;
    m_selection = selection;
    m_codeBackground = codeBg;
    m_linkColor = link;
    m_borderColor = border;

    if (changedAny)
        emit changed();
}
