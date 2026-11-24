#pragma once

#include <QObject>
#include <QUrl>

// Abstract open/save picker so Backend stays decoupled from D-Bus in tests.
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    ~FilePicker() override = default;

    virtual void openDocument(const QString &folder = {}) = 0;
    virtual void saveDocument(const QUrl &suggestedUrl) = 0;

signals:
    void openSelected(const QUrl &url);
    void saveSelected(const QUrl &url);
    void cancelled(bool forSave);
    void failed(const QString &message, bool forSave);
};
