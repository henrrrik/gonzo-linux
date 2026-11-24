// PortalFilePicker adapted from Omacut / Omawrite (MIT, David Heinemeier Hansson).
#include "portalfilepicker.h"

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFileInfo>
#include <QUuid>

struct PortalFilterRule {
    uint type = 0; // 0 = glob pattern
    QString pattern;
};

struct PortalFilter {
    QString name;
    QList<PortalFilterRule> rules;
};

Q_DECLARE_METATYPE(PortalFilterRule)
Q_DECLARE_METATYPE(QList<PortalFilterRule>)
Q_DECLARE_METATYPE(PortalFilter)
Q_DECLARE_METATYPE(QList<PortalFilter>)

static QDBusArgument &operator<<(QDBusArgument &arg, const PortalFilterRule &rule) {
    arg.beginStructure();
    arg << rule.type << rule.pattern;
    arg.endStructure();
    return arg;
}

static const QDBusArgument &operator>>(const QDBusArgument &arg, PortalFilterRule &rule) {
    arg.beginStructure();
    arg >> rule.type >> rule.pattern;
    arg.endStructure();
    return arg;
}

static QDBusArgument &operator<<(QDBusArgument &arg, const PortalFilter &filter) {
    arg.beginStructure();
    arg << filter.name << filter.rules;
    arg.endStructure();
    return arg;
}

static const QDBusArgument &operator>>(const QDBusArgument &arg, PortalFilter &filter) {
    arg.beginStructure();
    arg >> filter.name >> filter.rules;
    arg.endStructure();
    return arg;
}

namespace {

void registerPortalTypes() {
    static bool registered = false;
    if (registered)
        return;
    registered = true;
    qDBusRegisterMetaType<PortalFilterRule>();
    qDBusRegisterMetaType<QList<PortalFilterRule>>();
    qDBusRegisterMetaType<PortalFilter>();
    qDBusRegisterMetaType<QList<PortalFilter>>();
}

QList<PortalFilter> markdownFilters() {
    return {
        {QObject::tr("Markdown files"), {{0, QStringLiteral("*.md")}, {0, QStringLiteral("*.markdown")}, {0, QStringLiteral("*.mdown")}}},
        {QObject::tr("Plain text files"), {{0, QStringLiteral("*.txt")}}},
        {QObject::tr("All files"), {{0, QStringLiteral("*")}}}
    };
}

QByteArray portalFolderBytes(const QString &folder) {
    QByteArray encoded = QFile::encodeName(folder);
    encoded.append('\0');
    return encoded;
}

} // namespace

PortalFilePicker::PortalFilePicker(QObject *parent, const QDBusConnection &bus)
    : FilePicker(parent), m_bus(bus) {
    registerPortalTypes();
}

PortalFilePicker::~PortalFilePicker() {
    closePendingRequest();
}

void PortalFilePicker::openDocument(const QString &folder) {
    QVariantMap options;
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("multiple"), false);
    options.insert(QStringLiteral("filters"), QVariant::fromValue(markdownFilters()));
    if (!folder.isEmpty() && QDir(folder).exists())
        options.insert(QStringLiteral("current_folder"), portalFolderBytes(folder));
    requestFile(QStringLiteral("OpenFile"), tr("Open Document"), std::move(options), Action::Open);
}

void PortalFilePicker::saveDocument(const QUrl &suggestedUrl) {
    QVariantMap options;
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("filters"), QVariant::fromValue(markdownFilters()));

    const QString local = suggestedUrl.isLocalFile() ? suggestedUrl.toLocalFile() : QString();
    if (!local.isEmpty()) {
        const QFileInfo info(local);
        options.insert(QStringLiteral("current_name"), info.fileName());
        if (QDir(info.absolutePath()).exists())
            options.insert(QStringLiteral("current_folder"), portalFolderBytes(info.absolutePath()));
    } else {
        options.insert(QStringLiteral("current_name"), QStringLiteral("Untitled.md"));
        options.insert(QStringLiteral("current_folder"), portalFolderBytes(QDir::homePath()));
    }
    requestFile(QStringLiteral("SaveFile"), tr("Save Document"), std::move(options), Action::Save);
}

void PortalFilePicker::requestFile(const QString &method, const QString &title,
                                   QVariantMap options, Action action) {
    closePendingRequest();
    const bool forSave = (action == Action::Save);
    if (!m_bus.isConnected()) {
        emit failed(tr("Desktop file chooser portal is unavailable."), forSave);
        return;
    }

    const QString token = QStringLiteral("gonzo_") + QUuid::createUuid().toString(QUuid::Id128);
    options.insert(QStringLiteral("handle_token"), token);

    const QString sender = m_bus.baseService().mid(1).replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString expectedPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
                                     .arg(sender, token);
    m_pendingAction = action;
    ++m_pendingGeneration;
    const int generation = m_pendingGeneration;
    connectToRequestPath(expectedPath);

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.FileChooser"),
        method);
    msg << QString() << title << options;

    QDBusPendingCall call = m_bus.asyncCall(msg);
    m_pendingWatcher = new QDBusPendingCallWatcher(call, this);
    connect(m_pendingWatcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation, forSave](QDBusPendingCallWatcher *watcher) {
                const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
                if (watcher == m_pendingWatcher)
                    m_pendingWatcher = nullptr;
                watcher->deleteLater();
                if (generation != m_pendingGeneration)
                    return;
                if (reply.isError()) {
                    clearPending();
                    emit failed(tr("Could not open file chooser: %1").arg(reply.error().message()), forSave);
                    return;
                }
                const QString actualPath = reply.value().path();
                if (!actualPath.isEmpty() && actualPath != m_pendingPath)
                    connectToRequestPath(actualPath);
            });
}

bool PortalFilePicker::connectToRequestPath(const QString &path) {
    if (!m_pendingPath.isEmpty()) {
        m_bus.disconnect(QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
                         QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                         this, SLOT(handleResponse(uint,QVariantMap)));
    }
    m_pendingPath = path;
    return m_bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
                         QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                         this, SLOT(handleResponse(uint,QVariantMap)));
}

void PortalFilePicker::clearPending() {
    if (!m_pendingPath.isEmpty()) {
        m_bus.disconnect(QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
                         QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                         this, SLOT(handleResponse(uint,QVariantMap)));
        m_pendingPath.clear();
    }
    m_pendingAction = Action::None;
}

void PortalFilePicker::closePendingRequest() {
    const Action prevAction = m_pendingAction;
    if (m_pendingWatcher) {
        m_pendingWatcher->deleteLater();
        m_pendingWatcher = nullptr;
    }
    if (!m_pendingPath.isEmpty()) {
        QDBusMessage closeMsg = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"),
            m_pendingPath,
            QStringLiteral("org.freedesktop.portal.Request"),
            QStringLiteral("Close"));
        m_bus.asyncCall(closeMsg);
    }
    clearPending();
    if (prevAction != Action::None)
        emit cancelled(prevAction == Action::Save);
}

void PortalFilePicker::handleResponse(uint response, const QVariantMap &results) {
    const Action action = m_pendingAction;
    const bool forSave = (action == Action::Save);
    clearPending();

    if (response == 1) {
        emit cancelled(forSave);
        return;
    }
    if (response != 0) {
        emit failed(tr("File chooser failed."), forSave);
        return;
    }

    const QStringList uris = results.value(QStringLiteral("uris")).toStringList();
    if (uris.isEmpty()) {
        emit cancelled(forSave);
        return;
    }

    const QUrl url(uris.first());
    if (!url.isLocalFile()) {
        emit failed(tr("Gonzo opens regular local files."), forSave);
        return;
    }

    if (action == Action::Open)
        emit openSelected(url);
    else if (action == Action::Save)
        emit saveSelected(url);
}
