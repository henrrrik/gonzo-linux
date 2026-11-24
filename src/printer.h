#pragma once

#include <QObject>
#include <QString>

// Abstract printer interface so Backend can print plain monospace Markdown
// source without pulling in Qt Widgets (keeping pure QGuiApplication).
class Printer : public QObject {
    Q_OBJECT

public:
    explicit Printer(QObject *parent = nullptr) : QObject(parent) {}
    ~Printer() override = default;

    virtual void printMarkdownSource(const QString &title, const QString &text) = 0;

signals:
    void printSucceeded();
    void printCancelled();
    void printFailed(const QString &message);
};
