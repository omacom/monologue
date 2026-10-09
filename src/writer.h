#pragma once
#include <QObject>
#include <QAudioBufferInput>
#include <QVideoFrameInput>
#include <QMediaCaptureSession>
#include <QMediaRecorder>
#include <QVideoFrame>
#include <QQueue>
#include <QTimer>
#include <optional>

// Maps a monotonic capture clock onto a take with the paused intervals removed.
class TakeClock {
public:
    struct Span { qint64 begin, end, position; };
    void start(qint64 now);
    void pause(qint64 now);
    void resume(qint64 now);
    qint64 duration(qint64 now) const;
    std::optional<qint64> position(qint64 capturedAt) const;
    QList<Span> spans(qint64 begin, qint64 end) const;
    // Take positions where a pause was resumed.
    QList<qint64> joins() const;
    bool paused() const { return m_paused; }
    qint64 intervalStart() const { return m_start; }
private:
    struct Interval { qint64 begin, end, position; };
    QList<Interval> m_intervals;
    qint64 m_start = 0, m_completed = 0;
    bool m_paused = false;
};

class Writer : public QObject {
    Q_OBJECT
public:
    explicit Writer(QObject *parent = nullptr);
    bool start(const QString &path, const QVideoFrameFormat &videoFormat,
               double fps, const QAudioFormat &audioFormat, qint64 now);
    void video(QVideoFrame frame, qint64 capturedAt);
    void audio(QByteArray data, qint64 capturedAt);
    void pause(qint64 now);
    void resume(qint64 now);
    void finish();
    qint64 duration(qint64 now) const { return m_clock.duration(now); }
    QList<qint64> joins() const { return m_clock.joins(); }
    static bool supported(bool audio);
signals:
    void finished();
    void failed(const QString &message);
private:
    void drain();
    void fail(const QString &message);
    void appendAudio(const QByteArray &data);
    void padAudioTo(qint64 time);
    void videoAt(QVideoFrame frame, qint64 position);
    QMediaCaptureSession m_session;
    QMediaRecorder m_recorder;
    QVideoFrameInput *m_video = nullptr;
    QAudioBufferInput *m_audio = nullptr;
    QAudioFormat m_audioFormat;
    QQueue<QVideoFrame> m_frames;
    QVideoFrame m_tailFrame;
    QQueue<QAudioBuffer> m_buffers;
    QTimer m_flushTimeout;
    TakeClock m_clock;
    qint64 m_lastVideo = -1, m_lastIndex = -1;
    qint64 m_frameDuration = 33333;
    double m_fps = 30;
    qint64 m_queuedAudioUs = 0;
    qint64 m_audioFramesWritten = 0;
    bool m_stopping = false, m_failed = false, m_draining = false;
};
