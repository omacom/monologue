#include "writer.h"
#include <QMetaObject>
#include <cstring>
#include <QVideoFrameFormat>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}
#include <QMediaFormat>
#include <QUrl>
#include <QAbstractVideoBuffer>
#include <limits>

namespace {
// QVideoFrame copies share timestamp metadata as well as pixels. Give recording
// frames independent metadata without copying their image planes or altering
// the live preview (or an already queued copy of a pause's final frame).
class FrameBuffer : public QAbstractVideoBuffer {
public:
    explicit FrameBuffer(const QVideoFrame &source) : m_source(source) {}
    ~FrameBuffer() override { unmap(); }
    QVideoFrameFormat format() const override { return m_source.surfaceFormat(); }
    MapData map(QVideoFrame::MapMode mode) override {
        MapData result;
        if(!m_source.map(mode)) return result;
        m_mapped=true;
        result.planeCount=m_source.planeCount();
        for(int i=0;i<result.planeCount;++i) {
            result.data[i]=m_source.bits(i);
            result.bytesPerLine[i]=m_source.bytesPerLine(i);
            result.dataSize[i]=m_source.mappedBytes(i);
        }
        return result;
    }
    void unmap() override { if(m_mapped) { m_source.unmap(); m_mapped=false; } }
private:
    QVideoFrame m_source;
    bool m_mapped=false;
};

// Webcams deliver MJPEG, which Qt's encoder decodes to 4:4:4 that hardware H.264
// encoders reject and software x264 struggles with at 1080p. Hand the encoder
// plain 8-bit 4:2:0 NV12 instead, which also plays everywhere. Runs on a worker.
QVideoFrame jpegToNv12(const QByteArray &jpeg, const QSize &size) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec) return {};
    AVCodecContext *decoder = avcodec_alloc_context3(codec);
    AVPacket *packet = av_packet_alloc();
    AVFrame *decoded = av_frame_alloc();
    QVideoFrame output;
    if (decoder && packet && decoded && avcodec_open2(decoder, codec, nullptr) == 0
        && av_new_packet(packet, jpeg.size()) == 0) {
        memcpy(packet->data, jpeg.constData(), jpeg.size());
        if (avcodec_send_packet(decoder, packet) == 0 && avcodec_receive_frame(decoder, decoded) == 0
            && decoded->width == size.width() && decoded->height == size.height()) {
            QVideoFrameFormat format(size, QVideoFrameFormat::Format_NV12);
            format.setColorSpace(QVideoFrameFormat::ColorSpace_BT601);
            format.setColorRange(QVideoFrameFormat::ColorRange_Video);
            QVideoFrame frame(format);
            if (frame.map(QVideoFrame::WriteOnly)) {
                thread_local SwsContext *context = nullptr;
                const bool fullRange = decoded->color_range == AVCOL_RANGE_JPEG || decoded->format == AV_PIX_FMT_YUVJ420P
                    || decoded->format == AV_PIX_FMT_YUVJ422P || decoded->format == AV_PIX_FMT_YUVJ444P;
                AVPixelFormat source = AVPixelFormat(decoded->format);
                if (source == AV_PIX_FMT_YUVJ420P) source = AV_PIX_FMT_YUV420P;
                else if (source == AV_PIX_FMT_YUVJ422P) source = AV_PIX_FMT_YUV422P;
                else if (source == AV_PIX_FMT_YUVJ444P) source = AV_PIX_FMT_YUV444P;
                context = sws_getCachedContext(context, size.width(), size.height(), source,
                                               size.width(), size.height(), AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
                uint8_t *planes[2] = {frame.bits(0), frame.bits(1)};
                const int strides[2] = {frame.bytesPerLine(0), frame.bytesPerLine(1)};
                int rows = 0;
                if (context) {
                    const int *coefficients = sws_getCoefficients(SWS_CS_ITU601);
                    sws_setColorspaceDetails(context, coefficients, fullRange ? 1 : 0, coefficients, 0, 0, 1 << 16, 1 << 16);
                    rows = sws_scale(context, decoded->data, decoded->linesize, 0, size.height(), planes, strides);
                }
                frame.unmap();
                if (rows == size.height()) output = frame;
            }
        }
    }
    av_frame_free(&decoded);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    return output;
}
}

void TakeClock::start(qint64 now) {
    m_start = now; m_completed = 0; m_paused = false;
    m_intervals = {{now, std::numeric_limits<qint64>::max(), 0}};
}
void TakeClock::pause(qint64 now) {
    if (!m_paused) {
        m_intervals.last().end = std::max(now, m_start);
        m_completed += m_intervals.last().end - m_start;
        m_paused = true;
    }
}
void TakeClock::resume(qint64 now) {
    if (m_paused) {
        m_start = now; m_paused = false;
        m_intervals.append({now, std::numeric_limits<qint64>::max(), m_completed});
    }
}
qint64 TakeClock::duration(qint64 now) const { return m_completed + (m_paused ? 0 : std::max(qint64(0), now - m_start)); }
std::optional<qint64> TakeClock::position(qint64 time) const {
    for (auto i = m_intervals.crbegin(); i != m_intervals.crend(); ++i) {
        if (time < i->begin) continue;
        if (time < i->end) return i->position + time - i->begin;
        break;
    }
    return {};
}
QList<TakeClock::Span> TakeClock::spans(qint64 begin, qint64 end) const {
    QList<Span> result;
    for (auto i = m_intervals.crbegin(); i != m_intervals.crend(); ++i) {
        if (i->end <= begin) break;
        const qint64 first = std::max(begin, i->begin), last = std::min(end, i->end);
        if (first < last) result.prepend({first, last, i->position + first - i->begin});
    }
    return result;
}
QList<qint64> TakeClock::joins() const {
    QList<qint64> result;
    for (int i = 1; i < m_intervals.size(); ++i)
        if (m_intervals[i].position > 0 && (result.isEmpty() || result.last() != m_intervals[i].position)) result.append(m_intervals[i].position);
    return result;
}

Writer::Writer(QObject *parent) : QObject(parent) {
    m_convertPool.setMaxThreadCount(3);
    m_session.setRecorder(&m_recorder);
    m_flushTimeout.setSingleShot(true);
    m_flushTimeout.setInterval(10000);
    connect(&m_flushTimeout, &QTimer::timeout, this, [this] { fail("The encoder stopped responding, so the take was stopped."); });
    connect(&m_recorder, &QMediaRecorder::errorOccurred, this, [this](auto, const QString &message) { fail(message); });
    connect(&m_recorder, &QMediaRecorder::recorderStateChanged, this, [this](auto state) {
        if (state == QMediaRecorder::StoppedState && m_stopping) {
            m_flushTimeout.stop();
            emit finished();
        }
    });
}
static QMediaFormat recordingFormat(bool audio) {
    QMediaFormat f(QMediaFormat::MPEG4);
    f.setVideoCodec(QMediaFormat::VideoCodec::H264);
    if (audio) f.setAudioCodec(QMediaFormat::AudioCodec::AAC);
    return f;
}
Writer::~Writer() { m_convertPool.waitForDone(); }
bool Writer::supported(bool audio) { return recordingFormat(audio).isSupported(QMediaFormat::Encode); }
bool Writer::start(const QString &path, const QVideoFrameFormat &format,
                   double fps, const QAudioFormat &audioFormat, qint64 now) {
    m_clock.start(now);
    m_frameDuration = qRound64(1000000.0 / fps);
    m_fps = fps;
    m_audioFormat = audioFormat;
    // Let the first frame initialize the encoder. Supplying a format hint can
    // leave Qt 6.11's canPushFrame cache false before its worker starts.
    m_video = new QVideoFrameInput(this);
    m_session.setVideoFrameInput(m_video);
    connect(m_video, &QVideoFrameInput::readyToSendVideoFrame, this, &Writer::drain);
    if (audioFormat.isValid()) {
        m_audio = new QAudioBufferInput(this);
        m_session.setAudioBufferInput(m_audio);
        connect(m_audio, &QAudioBufferInput::readyToSendAudioBuffer, this, &Writer::drain);
    }
    m_recorder.setMediaFormat(recordingFormat(audioFormat.isValid()));
    m_recorder.setVideoResolution(format.frameSize());
    m_recorder.setVideoFrameRate(fps);
    m_recorder.setQuality(QMediaRecorder::VeryHighQuality);
    m_recorder.setAudioBitRate(192000);
    m_recorder.setOutputLocation(QUrl::fromLocalFile(path));
    m_recorder.record();
    return !m_failed;
}
void Writer::pause(qint64 now) {
    // Close the interval, but still accept buffers captured before this edge.
    // Padding now would overwrite the microphone's in-flight tail with silence.
    if (!m_stopping) m_clock.pause(now);
}
void Writer::resume(qint64 now) { if (!m_stopping) m_clock.resume(now); }
void Writer::video(QVideoFrame frame, qint64 capturedAt) {
    if (m_stopping || m_failed || !frame.isValid()) return;
    const auto time = m_clock.position(capturedAt);
    if (!time) return;
    videoAt(frame, *time);
}
void Writer::videoAt(QVideoFrame frame, qint64 position) {
    // Quantize to the output rate instead of rejecting short callback intervals:
    // real webcams deliver jittery batches, which must not halve the frame rate.
    const qint64 frameIndex = qRound64(position * m_fps / 1000000.0);
    const qint64 timestamp = qRound64(frameIndex * 1000000.0 / m_fps);
    if (timestamp <= m_lastVideo) return;
    // Bounded native-frame references; never build an unbounded 4K frame queue.
    if (m_frames.size() >= 6) { fail("Video encoding cannot keep up at this camera's maximum resolution, so the take was stopped."); return; }
    m_tailFrame = frame;
    m_lastVideo = timestamp;
    auto job = std::make_shared<Job>();
    bool queued = false;
    if (frame.pixelFormat() == QVideoFrameFormat::Format_Jpeg) {
        QByteArray jpeg;
        if (frame.map(QVideoFrame::ReadOnly)) {
            jpeg = QByteArray(reinterpret_cast<const char *>(frame.bits(0)), frame.mappedBytes(0));
            frame.unmap();
        }
        if (jpeg.isEmpty()) return;
        const QSize size = frame.size();
        queued = true;
        m_convertPool.start([this, job, jpeg, size] {
            job->frame = jpegToNv12(jpeg, size);
            job->ready.store(true, std::memory_order_release);
            QMetaObject::invokeMethod(this, &Writer::drain, Qt::QueuedConnection);
        });
    } else {
        job->frame = QVideoFrame(std::make_unique<FrameBuffer>(frame));
        job->ready.store(true, std::memory_order_release);
    }
    m_frames.enqueue({job, timestamp, qRound64((frameIndex + 1) * 1000000.0 / m_fps), {}});
    if (!queued) drain();
}
void Writer::audio(QByteArray data, qint64 capturedAt) {
    if (m_stopping || m_failed || !m_audio || data.isEmpty()) return;
    const qint64 end = capturedAt + m_audioFormat.durationForBytes(data.size());
    const int frameBytes = m_audioFormat.bytesPerFrame();
    // Select by capture time, including closed intervals. A late buffer may
    // straddle Pause, Resume, or Finish; only the recorded sample ranges survive.
    for (const auto &span : m_clock.spans(capturedAt, end)) {
        const auto sampleAtOrAfter = [this](qint64 time) {
            return (time * m_audioFormat.sampleRate() + 999999) / 1000000;
        };
        const qint64 first = sampleAtOrAfter(span.begin - capturedAt);
        const qint64 last = sampleAtOrAfter(span.end - capturedAt);
        QByteArray part = data.mid(first * frameBytes, (last - first) * frameBytes);
        const auto position = span.position + m_audioFormat.durationForFrames(first) - (span.begin - capturedAt);
        // Qt counts audio samples rather than honoring PTS. Materialize capture
        // gaps as silence and trim overlaps, keeping the original event times.
        const qint64 desiredFrame = qRound64(position * m_audioFormat.sampleRate() / 1000000.0);
        if (desiredFrame < m_audioFramesWritten) {
            const auto overlap = (m_audioFramesWritten - desiredFrame) * frameBytes;
            part.remove(0, std::min(qsizetype(part.size()), qsizetype(overlap)));
        } else padAudioTo(position);
        if (!part.isEmpty()) appendAudio(part);
    }
}
void Writer::padAudioTo(qint64 time) {
    if (!m_audio || m_failed) return;
    const qint64 missing = qRound64(time * m_audioFormat.sampleRate() / 1000000.0) - m_audioFramesWritten;
    if (missing <= 0) return;
    if (m_audioFormat.durationForFrames(missing) > 500000) {
        fail("Audio capture fell behind the recording, so the take was stopped."); return;
    }
    appendAudio(QByteArray(missing * m_audioFormat.bytesPerFrame(),
                          m_audioFormat.sampleFormat() == QAudioFormat::UInt8 ? char(128) : char(0)));
}
void Writer::appendAudio(const QByteArray &data) {
    if (m_failed) return;
    QAudioBuffer buffer(data, m_audioFormat, m_audioFormat.durationForFrames(m_audioFramesWritten));
    m_audioFramesWritten += buffer.frameCount();
    m_queuedAudioUs += buffer.duration();
    if (m_queuedAudioUs > 500000) { fail("Audio encoding cannot keep up, so the take was stopped."); return; }
    m_buffers.enqueue(buffer);
    drain();
}
void Writer::drain() {
    if (m_draining || m_failed) return;
    m_draining = true;
    while (!m_frames.isEmpty() && m_frames.head().job->ready.load(std::memory_order_acquire)) {
        auto &head = m_frames.head();
        if (!head.job->frame.isValid()) { m_frames.dequeue(); continue; }  // undecodable camera frame
        if (!head.frame.isValid()) {
            head.frame = QVideoFrame(std::make_unique<FrameBuffer>(head.job->frame));
            head.frame.setStartTime(head.start);
            head.frame.setEndTime(head.end);
            head.frame.setStreamFrameRate(m_fps);
        }
        if (!m_video->sendVideoFrame(head.frame)) break;
        m_frames.dequeue();
    }
    while (!m_buffers.isEmpty() && m_audio->sendAudioBuffer(m_buffers.head())) {
        m_queuedAudioUs -= m_buffers.head().duration(); m_buffers.dequeue();
    }
    m_draining = false;
    if (m_stopping && m_frames.isEmpty() && m_buffers.isEmpty()) m_recorder.stop();
}
void Writer::finish() {
    if (m_stopping) return;
    // Backend calls this after the capture watermarks pass Finish, so all
    // in-flight samples have had a chance to reach the closed final interval.
    if (m_clock.paused()) {
        const auto duration = m_clock.duration(0);
        if (m_tailFrame.isValid()) videoAt(m_tailFrame, std::max(qint64(0), duration - m_frameDuration));
        padAudioTo(duration);
    }
    if (m_failed) return;
    m_stopping = true;
    m_flushTimeout.start();
    drain();
    if (m_recorder.recorderState() == QMediaRecorder::StoppedState) {
        m_flushTimeout.stop();
        QTimer::singleShot(0, this, &Writer::finished);
    }
}
void Writer::fail(const QString &message) {
    if (m_failed) return;
    m_failed = true;
    m_stopping = true;
    emit failed(message);
    m_frames.clear(); m_buffers.clear();
    m_recorder.stop();
    if (m_recorder.recorderState() == QMediaRecorder::StoppedState)
        QTimer::singleShot(0, this, &Writer::finished);
}
