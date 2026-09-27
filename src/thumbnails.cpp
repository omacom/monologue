#include "thumbnails.h"
#include "mediautils.h"

Thumbnails::Thumbnails(QObject *parent) : QObject(parent) { m_pool.setMaxThreadCount(4); }
Thumbnails::~Thumbnails() { cancel(); m_pool.waitForDone(); }
void Thumbnails::cancel() {
    if (m_cancel) m_cancel->store(true);
    m_cancel.reset();
}
void Thumbnails::clear() {
    cancel(); m_path.clear(); m_duration = 0; m_whole.clear();
    m_images = QVector<QImage>(Count); m_store->reset(m_images); m_ready = 0; ++m_revision;
    emit changed();
}
void Thumbnails::load(const QString &path, double duration) {
    clear();
    m_path = path; m_duration = duration; m_start = 0; m_end = duration;
    generate();
}
void Thumbnails::request(double start, double end) {
    if (m_path.isEmpty() || end - start <= 0 || (start == m_start && end == m_end)) return;
    m_start = start; m_end = end;
    if (start <= 0 && end >= m_duration && !m_whole.isEmpty()) {
        cancel(); m_images = m_whole; m_store->reset(m_images); m_ready = Count; ++m_revision;
        emit changed(); return;
    }
    generate();
}
void Thumbnails::generate() {
    cancel();
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_images = QVector<QImage>(Count); m_store->reset(m_images); m_ready = 0; ++m_revision;
    emit changed();
    const int revision = m_revision;
    for (int i = 0; i < Count; ++i) {
        const double time = m_start + (m_end - m_start) * (i + .5) / Count;
        m_pool.start([this, revision, i, time, path = m_path, cancel = m_cancel] {
            if (cancel->load()) return;
            const auto image = media::thumbnail(path, time, 90, cancel.get());
            if (cancel->load()) return;
            QMetaObject::invokeMethod(this, [this, revision, i, image] { arrived(revision, i, image); }, Qt::QueuedConnection);
        });
    }
}
void Thumbnails::arrived(int revision, int index, const QImage &image) {
    if (revision != m_revision) return;
    m_images[index] = image.isNull() ? QImage(1, 1, QImage::Format_RGB32) : image;
    m_store->set(index, m_images[index]);
    // Reveal in order, so the strip fills from left to right.
    while (m_ready < Count && !m_images[m_ready].isNull()) ++m_ready;
    if (m_ready == Count && m_start <= 0 && m_end >= m_duration) m_whole = m_images;
    emit changed();
}
