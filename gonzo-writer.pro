QT += core gui qml quick quickcontrols2 dbus

CONFIG += c++17 release link_pkgconfig
PKGCONFIG += hunspell

TARGET = gonzo-writer
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/cli.h \
    src/documentstore.h \
    src/filepicker.h \
    src/highlighter.h \
    src/kineticscroller.h \
    src/markdown.h \
    src/portalfilepicker.h \
    src/portalprinter.h \
    src/printer.h \
    src/spellchecker.h \
    src/textmetrics.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/backend.cpp \
    src/cli.cpp \
    src/documentstore.cpp \
    src/highlighter.cpp \
    src/kineticscroller.cpp \
    src/main.cpp \
    src/markdown.cpp \
    src/portalfilepicker.cpp \
    src/portalprinter.cpp \
    src/spellchecker.cpp \
    src/textmetrics.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
