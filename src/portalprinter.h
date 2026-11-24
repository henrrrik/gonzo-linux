#pragma once

#include "printer.h"

#include <QDBusConnection>
#include <QTemporaryFile>
#include <QVariantMap>
#include <memory>

class QDBusPendingCallWatcher;

// Renders plain monospace Markdown source to a PDF via QPdfWriter (QtGui) and
// hands the file descriptor to org.freedesktop.portal.Print.
class PortalPrinter : public Printer {
    Q_OBJECT

public:
    explicit PortalPrinter(QObject *parent = nullptr,
                           const QDBusConnection &bus = QDBusConnection::sessionBus());
    ~PortalPrinter() override;

    void printMarkdownSource(const QString &title, const QString &text) override;

    // Exposed for tests to verify PDF generation without a D-Bus print portal.
    static bool renderPdf(const QString &text, QIODevice *device);

private slots:
    void handlePrepareResponse(uint response, const QVariantMap &results);
    void handlePrintResponse(uint response, const QVariantMap &results);

private:
    void cleanup();
    void sendPdfToPortal(uint token);

    QDBusConnection m_bus;
    QString m_title;
    QString m_text;
    QString m_requestPath;
    bool m_waitingForPrint = false;
    int m_generation = 0;
    QDBusPendingCallWatcher *m_watcher = nullptr;
    std::unique_ptr<QTemporaryFile> m_pdfFile;
};
