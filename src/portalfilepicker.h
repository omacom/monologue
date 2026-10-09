#pragma once

#include <QVariantMap>

#include "filepicker.h"

class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr);

    void saveVideo(const QUrl &suggestedUrl) override;

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    bool connectToRequestPath(const QString &path);
    void clearPending();

    QString m_pendingPath;
    bool m_pending = false;
};
