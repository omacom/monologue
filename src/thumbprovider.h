#pragma once
#include <QQuickImageProvider>
#include "thumbnails.h"

// Serves filmstrip images to QML as image://thumbs/<revision>/<index>.
class ThumbProvider : public QQuickImageProvider {
public:
    explicit ThumbProvider(std::shared_ptr<ThumbStore> store) : QQuickImageProvider(Image), m_store(std::move(store)) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requested) override {
        QImage image = m_store->get(id.section('/', -1).toInt());
        if (image.isNull()) return {};
        if (size) *size = image.size();
        if (requested.height() > 0 && image.height() > requested.height())
            image = image.scaledToHeight(requested.height(), Qt::SmoothTransformation);
        return image;
    }
private:
    std::shared_ptr<ThumbStore> m_store;
};
