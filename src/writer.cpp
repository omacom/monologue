#include "writer.h"
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
    qint64 frameIndex = qRound64(position * m_fps / 1000000.0);
    // When the camera's phase sits near the midpoint between output frames, its
    // jitter rounds consecutive frames onto one slot. Take the next slot instead,
    // provided that is at most three quarters of a frame early.
    if (frameIndex <= m_lastIndex && (m_lastIndex + 1) * 1000000.0 / m_fps - position <= 0.75 * 1000000.0 / m_fps)
        frameIndex = m_lastIndex + 1;
    const qint64 timestamp = qRound64(frameIndex * 1000000.0 / m_fps);
    if (timestamp <= m_lastVideo) return;
    m_lastIndex = frameIndex;
    m_tailFrame = frame;
    frame = QVideoFrame(std::make_unique<FrameBuffer>(frame));
    frame.setStartTime(timestamp);
    frame.setEndTime(qRound64((frameIndex + 1) * 1000000.0 / m_fps));
    frame.setStreamFrameRate(m_fps);
    m_lastVideo = timestamp;
    // Bounded native-frame references; never build an unbounded 4K frame queue.
    if (m_frames.size() >= 6) { fail("Video encoding cannot keep up at this camera's maximum resolution, so the take was stopped."); return; }
    m_frames.enqueue(frame);
    drain();
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
    while (!m_frames.isEmpty() && m_video->sendVideoFrame(m_frames.head())) m_frames.dequeue();
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
