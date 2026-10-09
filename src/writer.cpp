#include "writer.h"
#include "mediautils.h"
#include <QAbstractVideoBuffer>
#include <QtConcurrent>
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
    m_flushTimeout.setSingleShot(true);
    m_flushTimeout.setInterval(10000);
    connect(&m_flushTimeout, &QTimer::timeout, this, [this] {
        if (!m_failed) { m_failed = true; emit failed("The encoder stopped responding, so the take was stopped."); }
        emitFinished();
    });
}
Writer::~Writer() = default;
bool Writer::supported(bool audio) { return Encoder::supported(audio); }
bool Writer::start(const QString &path, const QVideoFrameFormat &format,
                   double fps, const QAudioFormat &audioFormat, qint64 now) {
    m_clock.start(now);
    m_frameDuration = qRound64(1000000.0 / fps);
    m_fps = fps;
    m_audioFormat = audioFormat;
    m_encoder = std::make_unique<Encoder>();
    const auto error = m_encoder->open({path, format.frameSize(), fps, audioFormat}, [this](const QString &message) {
        QMetaObject::invokeMethod(this, [this, message] { fail(message); }, Qt::QueuedConnection);
    });
    if (!error.isEmpty()) { m_encoder.reset(); fail(error); }
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
    m_tailFrame = frame;
    m_lastVideo = timestamp;
    // Bounded native-frame references; never build an unbounded 4K frame queue.
    if (m_frames.size() + m_encoder->queuedFrames() >= 8) {
        fail("Video encoding cannot keep up at this camera's maximum resolution, so the take was stopped."); return;
    }
    const auto stamp = [timestamp, end = qRound64((frameIndex + 1) * 1000000.0 / m_fps), fps = m_fps](QVideoFrame frame) {
        frame.setStartTime(timestamp);
        frame.setEndTime(end);
        frame.setStreamFrameRate(fps);
        return frame;
    };
    if (frame.pixelFormat() != QVideoFrameFormat::Format_Jpeg) {
        m_frames.enqueue(QtFuture::makeReadyValueFuture(stamp(QVideoFrame(std::make_unique<FrameBuffer>(frame)))));
        drain();
        return;
    }
    auto decoded = QtConcurrent::run([frame, stamp]() mutable {
        if (!frame.map(QVideoFrame::ReadOnly)) return QVideoFrame();
        auto result = media::decodeJpeg(QByteArrayView(frame.bits(0), frame.mappedBytes(0)));
        frame.unmap();
        return result.isValid() ? stamp(result) : result;
    });
    m_frames.enqueue(decoded);
    decoded.then(this, [this](const QVideoFrame &) { drain(); });
}
void Writer::audio(QByteArray data, qint64 capturedAt) {
    if (m_stopping || m_failed || !m_audioFormat.isValid() || data.isEmpty()) return;
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
        // The encoder counts audio samples rather than honoring PTS. Materialize
        // capture gaps as silence and trim overlaps, keeping the original event times.
        const qint64 desiredFrame = qRound64(position * m_audioFormat.sampleRate() / 1000000.0);
        if (desiredFrame < m_audioFramesWritten) {
            const auto overlap = (m_audioFramesWritten - desiredFrame) * frameBytes;
            part.remove(0, std::min(qsizetype(part.size()), qsizetype(overlap)));
        } else padAudioTo(position);
        if (!part.isEmpty()) appendAudio(part);
    }
}
void Writer::padAudioTo(qint64 time) {
    if (!m_audioFormat.isValid() || m_failed) return;
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
    if (m_encoder->queuedAudioUs() > 500000) { fail("Audio encoding cannot keep up, so the take was stopped."); return; }
    m_audioFramesWritten += m_audioFormat.framesForBytes(data.size());
    m_encoder->audio(data);
}
void Writer::drain() {
    if (m_failed || m_closing) return;
    while (!m_frames.isEmpty() && m_frames.head().isFinished()) {
        const auto frame = m_frames.dequeue().result();
        if (!frame.isValid()) { fail("A camera frame could not be decoded, so the take was stopped."); return; }
        m_encoder->video(frame);
    }
    if (m_stopping && m_frames.isEmpty()) close(false);
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
}
void Writer::close(bool discard) {
    if (m_closing) return;
    m_closing = true;
    if (!m_encoder) { QTimer::singleShot(0, this, &Writer::emitFinished); return; }
    m_encoder->finish(discard, [this](const QString &error) {
        QMetaObject::invokeMethod(this, [this, error] {
            if (!error.isEmpty() && !m_failed) { m_failed = true; emit failed(error); }
            emitFinished();
        }, Qt::QueuedConnection);
    });
}
void Writer::emitFinished() {
    m_flushTimeout.stop();
    if (m_finished) return;
    m_finished = true;
    emit finished();
}
void Writer::fail(const QString &message) {
    if (m_failed) return;
    m_failed = true;
    m_stopping = true;
    emit failed(message);
    m_frames.clear();
    // Keep what was already encoded: the file is still closed as a valid MP4.
    close(true);
}
