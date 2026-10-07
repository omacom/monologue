#pragma once

#include <QObject>
#include <QUrl>

// Chooses where to save a clip. An interface so tests can stand in for the portal.
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void saveVideo(const QUrl &suggestedUrl) = 0;

signals:
    void closed();
    void selected(const QUrl &url);
    void failed(const QString &message);
};
