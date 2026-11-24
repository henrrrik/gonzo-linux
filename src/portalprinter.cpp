#include "portalprinter.h"

#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QFontDatabase>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QTextDocument>
#include <QUuid>

#include <unistd.h>

PortalPrinter::PortalPrinter(QObject *parent, const QDBusConnection &bus)
    : Printer(parent), m_bus(bus) {}

PortalPrinter::~PortalPrinter() {
    cleanup();
}

bool PortalPrinter::renderPdf(const QString &text, QIODevice *device) {
    if (!device || !device->isOpen())
        return false;

    QPdfWriter writer(device);
    writer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4),
                                     QPageLayout::Portrait,
                                     QMarginsF(18, 18, 18, 18),
                                     QPageLayout::Millimeter));
    writer.setResolution(300);

    QTextDocument doc;
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(11.0);
    doc.setDefaultFont(mono);
    doc.setPlainText(text);
    doc.print(&writer);
    return true;
}

void PortalPrinter::printMarkdownSource(const QString &title, const QString &text) {
    cleanup();
    if (!m_bus.isConnected()) {
        emit printFailed(tr("Desktop print portal is unavailable."));
        return;
    }

    m_title = title.isEmpty() ? QStringLiteral("Gonzo") : title;
    m_text = text;
    m_waitingForPrint = false;
    ++m_generation;
    const int gen = m_generation;

    const QString token = QStringLiteral("gonzo_prep_") + QUuid::createUuid().toString(QUuid::Id128);
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    options.insert(QStringLiteral("modal"), true);

    const QString sender = m_bus.baseService().mid(1).replace(QLatin1Char('.'), QLatin1Char('_'));
    m_requestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
    m_bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), m_requestPath,
                  QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                  this, SLOT(handlePrepareResponse(uint,QVariantMap)));

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Print"),
        QStringLiteral("PreparePrint"));
    msg << QString() << m_title << QVariantMap{} << QVariantMap{} << options;

    m_watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(msg), this);
    connect(m_watcher, &QDBusPendingCallWatcher::finished, this,
            [this, gen](QDBusPendingCallWatcher *watcher) {
                const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
                if (watcher == m_watcher)
                    m_watcher = nullptr;
                watcher->deleteLater();
                if (gen != m_generation)
                    return;
                if (reply.isError()) {
                    cleanup();
                    emit printFailed(tr("Could not open print dialog: %1").arg(reply.error().message()));
                    return;
                }
                const QString actual = reply.value().path();
                if (!actual.isEmpty() && actual != m_requestPath) {
                    m_bus.disconnect(QStringLiteral("org.freedesktop.portal.Desktop"), m_requestPath,
                                     QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                                     this, SLOT(handlePrepareResponse(uint,QVariantMap)));
                    m_requestPath = actual;
                    m_bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), m_requestPath,
                                  QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                                  this, SLOT(handlePrepareResponse(uint,QVariantMap)));
                }
            });
}

void PortalPrinter::handlePrepareResponse(uint response, const QVariantMap &results) {
    if (!m_requestPath.isEmpty()) {
        m_bus.disconnect(QStringLiteral("org.freedesktop.portal.Desktop"), m_requestPath,
                         QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                         this, SLOT(handlePrepareResponse(uint,QVariantMap)));
        m_requestPath.clear();
    }

    if (response == 1) {
        cleanup();
        emit printCancelled();
        return;
    }
    if (response != 0) {
        cleanup();
        emit printFailed(tr("Print preparation failed."));
        return;
    }

    const uint printToken = results.value(QStringLiteral("token")).toUInt();
    sendPdfToPortal(printToken);
}

void PortalPrinter::sendPdfToPortal(uint token) {
    m_pdfFile = std::make_unique<QTemporaryFile>(QStringLiteral("gonzo-print-XXXXXX.pdf"));
    if (!m_pdfFile->open() || !renderPdf(m_text, m_pdfFile.get())) {
        cleanup();
        emit printFailed(tr("Could not render PDF for printing."));
        return;
    }
    m_pdfFile->flush();
    ::lseek(m_pdfFile->handle(), 0, SEEK_SET);

    m_waitingForPrint = true;
    ++m_generation;
    const int gen = m_generation;

    const QString handleToken = QStringLiteral("gonzo_print_") + QUuid::createUuid().toString(QUuid::Id128);
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), handleToken);
    options.insert(QStringLiteral("token"), token);
    options.insert(QStringLiteral("modal"), true);

    const QString sender = m_bus.baseService().mid(1).replace(QLatin1Char('.'), QLatin1Char('_'));
    m_requestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, handleToken);
    m_bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), m_requestPath,
                  QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                  this, SLOT(handlePrintResponse(uint,QVariantMap)));

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Print"),
        QStringLiteral("Print"));
    msg << QString() << m_title << QVariant::fromValue(QDBusUnixFileDescriptor(m_pdfFile->handle())) << options;

    m_watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(msg), this);
    connect(m_watcher, &QDBusPendingCallWatcher::finished, this,
            [this, gen](QDBusPendingCallWatcher *watcher) {
                const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
                if (watcher == m_watcher)
                    m_watcher = nullptr;
                watcher->deleteLater();
                if (gen != m_generation)
                    return;
                if (reply.isError()) {
                    cleanup();
                    emit printFailed(tr("Printing failed: %1").arg(reply.error().message()));
                }
            });
}

void PortalPrinter::handlePrintResponse(uint response, const QVariantMap &) {
    cleanup();
    if (response == 0)
        emit printSucceeded();
    else if (response == 1)
        emit printCancelled();
    else
        emit printFailed(tr("Printing failed."));
}

void PortalPrinter::cleanup() {
    if (m_watcher) {
        m_watcher->deleteLater();
        m_watcher = nullptr;
    }
    if (!m_requestPath.isEmpty()) {
        if (m_waitingForPrint) {
            m_bus.disconnect(QStringLiteral("org.freedesktop.portal.Desktop"), m_requestPath,
                             QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                             this, SLOT(handlePrintResponse(uint,QVariantMap)));
        } else {
            m_bus.disconnect(QStringLiteral("org.freedesktop.portal.Desktop"), m_requestPath,
                             QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                             this, SLOT(handlePrepareResponse(uint,QVariantMap)));
        }
        m_requestPath.clear();
    }
    m_pdfFile.reset();
    m_waitingForPrint = false;
}
