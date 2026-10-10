#pragma once
#include <QObject>
#include <QVideoFrame>

struct wl_display;
struct wl_registry;
struct wl_shm;
struct wl_buffer;
struct wl_callback;
struct hyprland_toplevel_export_frame_v1;
class QSocketNotifier;
class QTimer;

// Captures one Hyprland toplevel through hyprland_toplevel_export_v1 and
// wl_shm. Qt's QWindowCapture cannot see native Wayland windows on this
// compositor, and the screen-cast portal asks the user to pick a monitor.
class WindowCapture : public QObject {
    Q_OBJECT
public:
    explicit WindowCapture(QObject *parent = nullptr);
    ~WindowCapture() override;
    void start(quint32 handle);
    void stop();
signals:
    void frameReady(const QVideoFrame &frame);
    void failed(const QString &reason);
private:
    struct CaptureCallbacks;
    void bindGlobal(wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
    void noteSync(wl_callback *callback);
    void noteBuffer(uint32_t format, uint32_t width, uint32_t height, uint32_t stride);
    void noteFlags(uint32_t flags);
    void noteReady(uint32_t secHi, uint32_t secLo, uint32_t nsec);
    void noteFailed();
    void noteBufferDone();
    void onReadable();
    void onWritable();
    void flush();
    void afterDispatch();
    void requestFrame();
    void publish();
    bool ensureBuffer();
    void destroyFrame();
    void destroyBuffer();
    void fail(const QString &reason);
    wl_display *m_display = nullptr;
    wl_registry *m_registry = nullptr;
    wl_shm *m_shm = nullptr;
    struct hyprland_toplevel_export_manager_v1 *m_manager = nullptr;
    hyprland_toplevel_export_frame_v1 *m_frame = nullptr;
    wl_callback *m_sync = nullptr;
    wl_buffer *m_buffer = nullptr;
    QSocketNotifier *m_read = nullptr;
    QSocketNotifier *m_write = nullptr;
    QTimer *m_pace = nullptr;
    QTimer *m_watchdog = nullptr;
    QTimer *m_bindTimeout = nullptr;
    void *m_pixels = nullptr;
    size_t m_pixelsSize = 0;
    int m_poolFd = -1;
    quint32 m_handle = 0;
    uint32_t m_format = 0, m_width = 0, m_height = 0, m_stride = 0;
    uint32_t m_bufferFormat = 0, m_bufferWidth = 0, m_bufferHeight = 0, m_bufferStride = 0;
    uint32_t m_secHi = 0, m_secLo = 0, m_nsec = 0;
    QString m_pendingFailure;
    bool m_haveBuffer = false, m_tooLarge = false, m_yInvert = false, m_haveTimestamp = false;
    bool m_synced = false, m_capturing = false, m_ready = false, m_published = false;
    bool m_stopping = false, m_dispatching = false;
};
