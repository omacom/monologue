#pragma once
#include <QObject>
#include <QImage>
#include <QMutex>
#include <QThreadPool>
#include <QVector>
#include <atomic>
#include <memory>

// Filmstrip images, shared between the generator and QML's image provider.
class ThumbStore {
public:
    void reset(const QVector<QImage> &images) { QMutexLocker lock(&m_mutex); m_images = images; }
    void set(int index, const QImage &image) { QMutexLocker lock(&m_mutex); if (index >= 0 && index < m_images.size()) m_images[index] = image; }
    QImage get(int index) { QMutexLocker lock(&m_mutex); return index >= 0 && index < m_images.size() ? m_images[index] : QImage(); }
    QVector<QImage> all() { QMutexLocker lock(&m_mutex); return m_images; }
private:
    QMutex m_mutex;
    QVector<QImage> m_images;
};

// Extracts evenly spaced frames across the whole clip, or across the zoomed
// stretch, off the UI thread. The whole-clip strip is cached for zooming out.
class Thumbnails : public QObject {
    Q_OBJECT
    Q_PROPERTY(int count READ count CONSTANT)
    Q_PROPERTY(int ready READ ready NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    static constexpr int Count = 12;
    explicit Thumbnails(QObject *parent = nullptr);
    ~Thumbnails() override;
    int count() const { return Count; }
    int ready() const { return m_ready; }
    int revision() const { return m_revision; }
    std::shared_ptr<ThumbStore> store() const { return m_store; }
    void load(const QString &path, double duration);
    void clear();
    Q_INVOKABLE void request(double start, double end);
signals:
    void changed();
private:
    void generate();
    void cancel();
    void arrived(int revision, int index, const QImage &image);
    std::shared_ptr<ThumbStore> m_store = std::make_shared<ThumbStore>();
    std::shared_ptr<std::atomic<bool>> m_cancel;
    QThreadPool m_pool;
    QString m_path;
    double m_duration = 0, m_start = 0, m_end = 0;
    QVector<QImage> m_images, m_whole;
    int m_ready = 0, m_revision = 0;
};
