#pragma once

#include "filepicker.h"

#include <QDBusConnection>
#include <QVariantMap>

class QDBusPendingCallWatcher;

// Out-of-process open/save dialogs via org.freedesktop.portal.FileChooser.
// Pattern adapted from Omacut / Omawrite (MIT, David Heinemeier Hansson).
class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr,
                              const QDBusConnection &bus = QDBusConnection::sessionBus());
    ~PortalFilePicker() override;

    void openDocument(const QString &folder = {}) override;
    void saveDocument(const QUrl &suggestedUrl) override;

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    enum class Action { None, Open, Save };

    void requestFile(const QString &method, const QString &title, QVariantMap options, Action action);
    bool connectToRequestPath(const QString &path);
    void clearPending();
    void closePendingRequest();

    QDBusConnection m_bus;
    QString m_pendingPath;
    Action m_pendingAction = Action::None;
    int m_pendingGeneration = 0;
    QDBusPendingCallWatcher *m_pendingWatcher = nullptr;
};
