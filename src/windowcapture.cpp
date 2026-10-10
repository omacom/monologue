#include "windowcapture.h"
#include "hyprland-toplevel-export-v1-client-protocol.h"
#include <QImage>
#include <QSocketNotifier>
#include <QTimer>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <wayland-client.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

// The generated protocol table names this interface. Capture uses the hyprctl
// address, not a wlr toplevel handle, so the interface is never marshalled.
// `extern` keeps the const object visible to the C protocol file; a plain
// const at namespace scope would have internal linkage.
extern "C" const wl_interface zwlr_foreign_toplevel_handle_v1_interface = {
    "zwlr_foreign_toplevel_handle_v1", 3, 0, nullptr, 0, nullptr};

namespace {
QImage::Format shmImageFormat(uint32_t format) {
    switch (format) {
    case WL_SHM_FORMAT_ARGB8888: return QImage::Format_ARGB32_Premultiplied;
    case WL_SHM_FORMAT_XRGB8888: return QImage::Format_RGB32;
    case WL_SHM_FORMAT_ABGR8888: return QImage::Format_RGBA8888_Premultiplied;
    case WL_SHM_FORMAT_XBGR8888: return QImage::Format_RGBX8888;
    default: return QImage::Format_Invalid;
    }
}
}

struct WindowCapture::CaptureCallbacks {
    static void global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
        static_cast<WindowCapture *>(data)->bindGlobal(registry, name, interface, version);
    }
    static void globalRemove(void *, wl_registry *, uint32_t) {}
    static void syncDone(void *data, wl_callback *callback, uint32_t) {
        static_cast<WindowCapture *>(data)->noteSync(callback);
    }
    static void buffer(void *data, hyprland_toplevel_export_frame_v1 *, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
        static_cast<WindowCapture *>(data)->noteBuffer(format, width, height, stride);
    }
    static void damage(void *, hyprland_toplevel_export_frame_v1 *, uint32_t, uint32_t, uint32_t, uint32_t) {}
    static void flags(void *data, hyprland_toplevel_export_frame_v1 *, uint32_t flags) {
        static_cast<WindowCapture *>(data)->noteFlags(flags);
    }
    static void ready(void *data, hyprland_toplevel_export_frame_v1 *, uint32_t secHi, uint32_t secLo, uint32_t nsec) {
        static_cast<WindowCapture *>(data)->noteReady(secHi, secLo, nsec);
    }
    static void failed(void *data, hyprland_toplevel_export_frame_v1 *) {
        static_cast<WindowCapture *>(data)->noteFailed();
    }
    static void linuxDmabuf(void *, hyprland_toplevel_export_frame_v1 *, uint32_t, uint32_t, uint32_t) {}
    static void bufferDone(void *data, hyprland_toplevel_export_frame_v1 *) {
        static_cast<WindowCapture *>(data)->noteBufferDone();
    }
};

WindowCapture::WindowCapture(QObject *parent) : QObject(parent) {
    m_pace = new QTimer(this);
    m_pace->setSingleShot(true);
    m_pace->setInterval(33);
    connect(m_pace, &QTimer::timeout, this, &WindowCapture::requestFrame);
    m_watchdog = new QTimer(this);
    m_watchdog->setSingleShot(true);
    m_watchdog->setInterval(3000);
    connect(m_watchdog, &QTimer::timeout, this, [this] { fail("The window stopped delivering frames."); });
    m_bindTimeout = new QTimer(this);
    m_bindTimeout->setSingleShot(true);
    m_bindTimeout->setInterval(1500);
    connect(m_bindTimeout, &QTimer::timeout, this, [this] { fail("This compositor cannot capture a window."); });
}
WindowCapture::~WindowCapture() {
    m_stopping = true;
    stop();
}
void WindowCapture::start(quint32 handle) {
    m_stopping = true;
    stop();
    m_stopping = false;
    m_handle = handle;
    m_published = false;
    m_capturing = false;
    m_synced = false;
    m_ready = false;
    m_pendingFailure.clear();
    m_display = wl_display_connect(nullptr);
    if (!m_display) { fail("This compositor cannot capture a window."); return; }
    const int fd = wl_display_get_fd(m_display);
    m_registry = wl_display_get_registry(m_display);
    static const wl_registry_listener registryListener{CaptureCallbacks::global, CaptureCallbacks::globalRemove};
    wl_registry_add_listener(m_registry, &registryListener, this);
    m_sync = wl_display_sync(m_display);
    static const wl_callback_listener syncListener{CaptureCallbacks::syncDone};
    wl_callback_add_listener(m_sync, &syncListener, this);
    m_read = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    m_write = new QSocketNotifier(fd, QSocketNotifier::Write, this);
    m_write->setEnabled(false);
    connect(m_read, &QSocketNotifier::activated, this, [this](QSocketDescriptor, QSocketNotifier::Type) { onReadable(); });
    connect(m_write, &QSocketNotifier::activated, this, [this](QSocketDescriptor, QSocketNotifier::Type) { onWritable(); });
    flush();
    m_bindTimeout->start();
    m_watchdog->start();
}
void WindowCapture::stop() {
    m_pace->stop();
    m_watchdog->stop();
    m_bindTimeout->stop();
    auto retire = [this](QSocketNotifier *&notifier) {
        if (!notifier) return;
        notifier->setEnabled(false);
        notifier->disconnect(this);
        if (m_dispatching) notifier->deleteLater();
        else delete notifier;
        notifier = nullptr;
    };
    retire(m_read);
    retire(m_write);
    destroyFrame();
    destroyBuffer();
    if (m_sync) { wl_callback_destroy(m_sync); m_sync = nullptr; }
    if (m_manager) { hyprland_toplevel_export_manager_v1_destroy(m_manager); m_manager = nullptr; }
    if (m_shm) { wl_shm_destroy(m_shm); m_shm = nullptr; }
    if (m_registry) { wl_registry_destroy(m_registry); m_registry = nullptr; }
    if (m_display) {
        wl_display_flush(m_display);
        wl_display_disconnect(m_display);
        m_display = nullptr;
    }
}
void WindowCapture::bindGlobal(wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    if (std::strcmp(interface, wl_shm_interface.name) == 0 && !m_shm)
        m_shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, std::min(version, 1u)));
    else if (std::strcmp(interface, hyprland_toplevel_export_manager_v1_interface.name) == 0 && !m_manager)
        m_manager = static_cast<hyprland_toplevel_export_manager_v1 *>(wl_registry_bind(registry, name, &hyprland_toplevel_export_manager_v1_interface, std::min(version, 2u)));
}
void WindowCapture::noteSync(wl_callback *callback) {
    if (callback == m_sync) { wl_callback_destroy(callback); m_sync = nullptr; }
    m_synced = true;
}
void WindowCapture::noteBuffer(uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    m_format = format;
    m_width = width;
    m_height = height;
    m_stride = stride;
    m_tooLarge = width > 16384 || height > 16384;
    m_haveBuffer = width > 0 && height > 0 && !m_tooLarge && stride >= width * 4u && stride <= 16384u * 4u && stride % 4u == 0;
}
void WindowCapture::noteFlags(uint32_t flags) { m_yInvert = flags & HYPRLAND_TOPLEVEL_EXPORT_FRAME_V1_FLAGS_Y_INVERT; }
void WindowCapture::noteReady(uint32_t secHi, uint32_t secLo, uint32_t nsec) {
    m_secHi = secHi;
    m_secLo = secLo;
    m_nsec = nsec;
    m_haveTimestamp = true;
    m_ready = true;
}
void WindowCapture::noteFailed() {
    if (m_pendingFailure.isEmpty())
        m_pendingFailure = m_published ? QStringLiteral("The window closed, so the take was stopped.")
                                       : QStringLiteral("The window stopped delivering frames.");
}
void WindowCapture::noteBufferDone() {
    if (!m_pendingFailure.isEmpty()) return;
    if (!m_haveBuffer) {
        m_pendingFailure = m_tooLarge ? QStringLiteral("This window is too large to record.")
                                      : QStringLiteral("This compositor cannot capture a window.");
        return;
    }
    if (shmImageFormat(m_format) == QImage::Format_Invalid) {
        m_pendingFailure = QStringLiteral("This window's pixel format cannot be recorded.");
        return;
    }
    if (!m_frame || !ensureBuffer()) {
        m_pendingFailure = QStringLiteral("This compositor cannot capture a window.");
        return;
    }
    // ignore_damage must be set. A still window produces no damage, so the
    // compositor would otherwise wait forever and the watchdog would fire.
    hyprland_toplevel_export_frame_v1_copy(m_frame, m_buffer, 1);
}
void WindowCapture::onReadable() {
    if (!m_display || m_stopping) return;
    m_dispatching = true;
    if (wl_display_prepare_read(m_display) == 0) {
        pollfd ready{};
        ready.fd = wl_display_get_fd(m_display);
        ready.events = POLLIN;
        if (poll(&ready, 1, 0) > 0) {
            if (wl_display_read_events(m_display) < 0 && m_pendingFailure.isEmpty())
                m_pendingFailure = QStringLiteral("The window stopped delivering frames.");
        } else wl_display_cancel_read(m_display);
    }
    if (m_display && m_pendingFailure.isEmpty() && wl_display_dispatch_pending(m_display) < 0)
        m_pendingFailure = QStringLiteral("The window stopped delivering frames.");
    if (m_display && !m_stopping) flush();
    if (m_display && !m_stopping) afterDispatch();
    m_dispatching = false;
}
void WindowCapture::onWritable() {
    if (m_write) m_write->setEnabled(false);
    flush();
}
void WindowCapture::flush() {
    if (!m_display || m_stopping) return;
    while (wl_display_flush(m_display) < 0) {
        if (errno == EAGAIN) { if (m_write) m_write->setEnabled(true); return; }
        if (m_pendingFailure.isEmpty()) m_pendingFailure = QStringLiteral("The window stopped delivering frames.");
        return;
    }
}
void WindowCapture::afterDispatch() {
    if (!m_pendingFailure.isEmpty()) {
        const auto message = m_pendingFailure;
        m_pendingFailure.clear();
        fail(message);
        return;
    }
    if (m_synced && !m_capturing) {
        m_synced = false;
        m_bindTimeout->stop();
        if (!m_manager || !m_shm) { fail("This compositor cannot capture a window."); return; }
        m_capturing = true;
        requestFrame();
        return;
    }
    if (!m_ready) return;
    m_ready = false;
    publish();
    destroyFrame();
    if (m_stopping) return;
    m_watchdog->start();
    m_pace->start();
}
void WindowCapture::requestFrame() {
    if (m_stopping || !m_manager || m_frame) return;
    m_haveBuffer = false;
    m_tooLarge = false;
    m_yInvert = false;
    m_haveTimestamp = false;
    m_ready = false;
    m_frame = hyprland_toplevel_export_manager_v1_capture_toplevel(m_manager, 1, m_handle);
    static const hyprland_toplevel_export_frame_v1_listener listener{
        CaptureCallbacks::buffer, CaptureCallbacks::damage, CaptureCallbacks::flags, CaptureCallbacks::ready,
        CaptureCallbacks::failed, CaptureCallbacks::linuxDmabuf, CaptureCallbacks::bufferDone};
    hyprland_toplevel_export_frame_v1_add_listener(m_frame, &listener, this);
    flush();
}
void WindowCapture::publish() {
    const auto imageFormat = shmImageFormat(m_format);
    if (imageFormat == QImage::Format_Invalid || !m_pixels || !m_width || !m_height) return;
    QImage image(static_cast<const uchar *>(m_pixels), int(m_width), int(m_height), int(m_stride), imageFormat);
    image = image.copy();
    if (m_yInvert) image = image.flipped(Qt::Vertical);
    const bool alpha = imageFormat == QImage::Format_ARGB32_Premultiplied || imageFormat == QImage::Format_RGBA8888_Premultiplied;
    image = image.convertToFormat(alpha ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
    QVideoFrame frame(image);
    if (m_haveTimestamp && m_nsec < 1000000000u) {
        const quint64 sec = (quint64(m_secHi) << 32) | m_secLo;
        if (sec <= quint64(std::numeric_limits<qint64>::max() / 1000000))
            frame.setStartTime(qint64(sec) * 1000000 + qint64(m_nsec / 1000));
    }
    m_published = true;
    emit frameReady(frame);
}
bool WindowCapture::ensureBuffer() {
    if (m_buffer && m_bufferFormat == m_format && m_bufferWidth == m_width && m_bufferHeight == m_height && m_bufferStride == m_stride)
        return true;
    destroyBuffer();
    if (m_height != 0 && m_stride > std::numeric_limits<size_t>::max() / m_height) return false;
    const size_t bytes = size_t(m_stride) * m_height;
    m_poolFd = memfd_create("monologue-window", MFD_CLOEXEC);
    if (m_poolFd < 0) return false;
    if (ftruncate(m_poolFd, off_t(bytes)) < 0) { destroyBuffer(); return false; }
    m_pixels = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, m_poolFd, 0);
    if (m_pixels == MAP_FAILED) { m_pixels = nullptr; destroyBuffer(); return false; }
    m_pixelsSize = bytes;
    auto *pool = wl_shm_create_pool(m_shm, m_poolFd, int(bytes));
    if (!pool) { destroyBuffer(); return false; }
    m_buffer = wl_shm_pool_create_buffer(pool, 0, int(m_width), int(m_height), int(m_stride), m_format);
    wl_shm_pool_destroy(pool);
    if (!m_buffer) { destroyBuffer(); return false; }
    m_bufferFormat = m_format;
    m_bufferWidth = m_width;
    m_bufferHeight = m_height;
    m_bufferStride = m_stride;
    return true;
}
void WindowCapture::destroyFrame() {
    if (!m_frame) return;
    hyprland_toplevel_export_frame_v1_destroy(m_frame);
    m_frame = nullptr;
}
void WindowCapture::destroyBuffer() {
    if (m_buffer) { wl_buffer_destroy(m_buffer); m_buffer = nullptr; }
    if (m_pixels) { munmap(m_pixels, m_pixelsSize); m_pixels = nullptr; }
    m_pixelsSize = 0;
    m_bufferFormat = m_bufferWidth = m_bufferHeight = m_bufferStride = 0;
    if (m_poolFd >= 0) { close(m_poolFd); m_poolFd = -1; }
}
void WindowCapture::fail(const QString &reason) {
    if (m_stopping) return;
    m_stopping = true;
    stop();
    emit failed(reason);
}
