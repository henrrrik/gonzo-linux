// gonzo — distraction-free Markdown writing for Omarchy.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "backend.h"
#include "cli.h"
#include "kineticscroller.h"
#include "portalfilepicker.h"
#include "portalprinter.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    QStringList rawArgs;
    rawArgs.reserve(argc);
    for (int i = 0; i < argc; ++i)
        rawArgs.append(QString::fromLocal8Bit(argv[i]));

    // Headless CLI commands run without connecting to a display server.
    if (Cli::isCliCommand(rawArgs)) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QCoreApplication cliApp(argc, argv);
        cliApp.setApplicationName(QStringLiteral("gonzo"));
        cliApp.setApplicationVersion(QStringLiteral("0.1.0"));
        return Cli::run(cliApp.arguments());
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("gonzo"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    // Matches the window to gonzo.desktop so Wayland (app_id) and the Omarchy
    // launcher pick up the installed icon.
    app.setDesktopFileName(QStringLiteral("gonzo"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("gonzo")));

    QQuickStyle::setStyle(QStringLiteral("Material"));

    // Installed before anything is translated; QML re-evaluates qsTr() live on
    // Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    PortalFilePicker filePicker;
    PortalPrinter printer;
    KineticScroller scroller;
    Backend backend(&theme, &filePicker, &printer);
    backend.setLanguage(translator.language());

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    QObject::connect(&translator, &Translator::changed, &backend, [&translator, &backend] {
        backend.setLanguage(translator.language());
        backend.retranslate();
    });

    engine.rootContext()->setContextProperty(QStringLiteral("translator"), &translator);
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.rootContext()->setContextProperty(QStringLiteral("scroller"), &scroller);

    engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;

    if (const auto fileArg = Cli::fileArgumentForGui(app.arguments()); fileArg.has_value()) {
        backend.openOrStartFile(*fileArg);
    } else {
        backend.tryRecoverDraft();
    }

    return app.exec();
}
