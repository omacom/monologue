#pragma once
#include <QObject>
#include <QList>
#include <QSize>
#include <QString>
#include <QVideoFrame>
#include <QVideoFrameFormat>

struct pw_thread_loop;
struct pw_stream;

// A camera that only PipeWire can deliver. libcamera's cameras, such as the
// MIPI sensors in Snapdragon and Intel IPU laptops, have no V4L2 node that Qt
// can open, so Qt never lists them; WirePlumber exposes them as PipeWire nodes.
class PipeWireCamera : public QObject {
    Q_OBJECT
public:
    struct Source { QByteArray id; QString description; };
    // The libcamera cameras PipeWire offers now. Empty when PipeWire isn't running.
    static QList<Source> sources();
    // Whether a camera id (as sources() gives it) names a PipeWire camera.
    static bool isSource(const QByteArray &id);
    explicit PipeWireCamera(QObject *parent = nullptr);
    ~PipeWireCamera() override;
    void start(const QByteArray &id);
    void stop();
signals:
    // On the object's thread, after the format is known.
    void frameReady(const QVideoFrame &frame);
    void formatChanged(QSize size, double fps);
    void failed(const QString &message);
private:
    // The stream's callbacks, which run on the PipeWire thread.
    friend struct PipeWireCameraEvents;
    pw_thread_loop *m_loop = nullptr;
    pw_stream *m_stream = nullptr;
    // Written by the PipeWire thread while it holds the loop lock, before any frame.
    QVideoFrameFormat::PixelFormat m_pixelFormat = QVideoFrameFormat::Format_Invalid;
    QSize m_size;
    double m_fps = 30;
    bool m_streaming = false;
};
