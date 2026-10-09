#pragma once
#include <QAudioFormat>
#include <QByteArray>
#include <QSize>
#include <QString>
#include <QVideoFrame>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

struct AVCodecContext;
struct AVFormatContext;
struct AVStream;
struct AVBufferRef;
struct AVFrame;
struct AVAudioFifo;
struct SwsContext;
struct SwrContext;

// Encodes H.264/AAC into an MP4 on its own thread. Uses the GPU's VAAPI
// encoder when available, which sustains 4K30 where Qt's libx264 (always at
// its "medium" preset) cannot; otherwise libx264 at a realtime preset.
class Encoder {
public:
    struct Settings { QString path; QSize size; double fps = 30; QAudioFormat audio; };
    ~Encoder();
    // Opens the file and codecs and starts the worker. Returns an error, or empty.
    // failed runs on the worker thread at the first encoding error.
    QString open(const Settings &settings, std::function<void(const QString &)> failed);
    // Queues a frame; its start time, quantized to the frame rate, sets its position.
    void video(const QVideoFrame &frame);
    // Queues PCM in the input format, contiguous with all earlier audio.
    void audio(const QByteArray &pcm);
    // Flushes and closes the file, dropping queued input first if discard is set.
    // done runs on the worker thread with an error, or empty on success.
    void finish(bool discard, std::function<void(const QString &)> done);
    int queuedFrames() const { return m_queuedFrames; }
    qint64 queuedAudioUs() const { return m_queuedAudioUs; }
    QString videoEncoder() const { return m_videoEncoderName; }
    static bool supported(bool audio);
private:
    struct Job { QVideoFrame frame; QByteArray pcm; };
    QString openVideo(const Settings &settings, bool hardware);
    QString openAudio(const QAudioFormat &format);
    void run();
    QString encodeVideo(const QVideoFrame &source);
    QString encodeAudio(const QByteArray &pcm, bool flush);
    QString send(AVCodecContext *codec, AVStream *stream, AVFrame *frame);
    QString close();
    void release();
    AVFormatContext *m_format = nullptr;
    AVCodecContext *m_video = nullptr, *m_audio = nullptr;
    AVStream *m_videoStream = nullptr, *m_audioStream = nullptr;
    AVBufferRef *m_device = nullptr;
    AVFrame *m_software = nullptr;
    SwsContext *m_scale = nullptr;
    SwrContext *m_resample = nullptr;
    AVAudioFifo *m_fifo = nullptr;
    QAudioFormat m_audioFormat;
    QString m_videoEncoderName;
    qint64 m_audioSamples = 0;
    bool m_headerWritten = false;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_jobs;
    bool m_finishing = false, m_discard = false;
    std::function<void(const QString &)> m_done, m_failed;
    std::atomic<int> m_queuedFrames{0};
    std::atomic<qint64> m_queuedAudioUs{0};
};
