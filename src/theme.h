#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QSocketNotifier>
#include <QTimer>
#include <memory>

class QDBusVariant;

// Follows Omarchy's current theme (~/.local/state/omarchy/current/theme/colors.toml)
// live, including dark/light mode and palette colors, plus Hyprland's effective
// window rounding via hyprctl.
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool darkMode READ darkMode NOTIFY changed)
    Q_PROPERTY(QString background READ background NOTIFY changed)
    Q_PROPERTY(QString panelBackground READ panelBackground NOTIFY changed)
    Q_PROPERTY(QString foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QString muted READ muted NOTIFY changed)
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QString selection READ selection NOTIFY changed)
    Q_PROPERTY(QString selectionForeground READ selectionForeground NOTIFY changed)
    Q_PROPERTY(QString codeBackground READ codeBackground NOTIFY changed)
    Q_PROPERTY(QString linkColor READ linkColor NOTIFY changed)
    Q_PROPERTY(QString borderColor READ borderColor NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr,
                   const QString &hyprctl = {});
    ~Theme() override;

    bool darkMode() const { return m_darkMode; }
    QString background() const { return m_background; }
    QString panelBackground() const { return m_panelBackground; }
    QString foreground() const { return m_foreground; }
    QString muted() const { return m_muted; }
    QString accent() const { return m_accent; }
    QString accentForeground() const;
    QString selection() const { return m_selection; }
    QString selectionForeground() const;
    QString codeBackground() const { return m_codeBackground; }
    QString linkColor() const { return m_linkColor; }
    QString borderColor() const { return m_borderColor; }
    int radius() const { return m_radius; }

    static QString readAccent(const QString &path);
    static QString readableForeground(const QString &bgHex);

signals:
    void changed();

private slots:
    void handlePortalSettingChanged(const QString &nameSpace, const QString &key,
                                    const QDBusVariant &value);

private:
    void reload();
    void refreshRounding();
    void connectHyprlandEventSocket();
    void handleHyprlandEventSocket();
    bool detectPortalDarkMode(bool *known) const;

    QString m_directory;
    bool m_usePortal = false;
    bool m_portalDarkMode = true;

    bool m_darkMode = true;
    QString m_background = QStringLiteral("#0e0e10");
    QString m_panelBackground = QStringLiteral("#18181b");
    QString m_foreground = QStringLiteral("#e8e8ec");
    QString m_muted = QStringLiteral("#8a8a94");
    QString m_accent = QStringLiteral("#FFD60A");
    QString m_selection = QStringLiteral("#2a2a32");
    QString m_codeBackground = QStringLiteral("#1c1c22");
    QString m_linkColor = QStringLiteral("#FFD60A");
    QString m_borderColor = QStringLiteral("#2c2c34");

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll;
    QTimer m_roundingTimeout;
    int m_hyprEventFd = -1;
    std::unique_ptr<QSocketNotifier> m_hyprEventNotifier;
    QByteArray m_hyprEventBuffer;
};
