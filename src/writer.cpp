#include "writer.h"
#include <QMediaFormat>
#include <QUrl>
#include <QAbstractVideoBuffer>
#include <QImage>
#include <limits>
#include <memory>
#include <cstring>
#include <turbojpeg.h>

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

// Qt chooses the encoder's input format from the source frame's. For RGB it
// asks for 12-bit 4:4:4 and for packed 4:2:2 for 10-bit 4:2:2, both of which
// H.264 NVENC rejects; Qt then falls back to software x264 at 4:4:4 or 4:2:2,
// which cannot keep up at 4K and starves the audio queue. Hand the encoder
// 8-bit 4:2:0 instead, so hardware encoders apply and software encoding stays
// cheap. Webcams send their largest modes as MJPEG, which is decoded straight
// to YUV. map() runs on Qt's encoder thread, keeping the work off the UI thread.
class Yuv420FrameBuffer : public QAbstractVideoBuffer {
public:
    static bool converts(QVideoFrameFormat::PixelFormat format) {
        switch (format) {
        case QVideoFrameFormat::Format_Jpeg:
        case QVideoFrameFormat::Format_YUYV:
        case QVideoFrameFormat::Format_UYVY:
        case QVideoFrameFormat::Format_ARGB8888: case QVideoFrameFormat::Format_ARGB8888_Premultiplied:
        case QVideoFrameFormat::Format_XRGB8888: case QVideoFrameFormat::Format_BGRA8888:
        case QVideoFrameFormat::Format_BGRA8888_Premultiplied: case QVideoFrameFormat::Format_BGRX8888:
        case QVideoFrameFormat::Format_ABGR8888: case QVideoFrameFormat::Format_XBGR8888:
        case QVideoFrameFormat::Format_RGBA8888: case QVideoFrameFormat::Format_RGBX8888:
            return true;
        default:
            return false;
        }
    }
    explicit Yuv420FrameBuffer(const QVideoFrame &source)
        : m_source(source),
          m_format(source.size(), QVideoFrameFormat::Format_YUV420P) {
        const bool jpeg = source.pixelFormat() == QVideoFrameFormat::Format_Jpeg;
        m_format.setColorSpace(QVideoFrameFormat::ColorSpace_BT601);
        m_format.setColorRange(jpeg ? QVideoFrameFormat::ColorRange_Full : QVideoFrameFormat::ColorRange_Video);
        m_format.setStreamFrameRate(source.surfaceFormat().streamFrameRate());
    }
    QVideoFrameFormat format() const override { return m_format; }
    MapData map(QVideoFrame::MapMode mode) override {
        MapData result;
        if(mode != QVideoFrame::ReadOnly) return result;
        if(m_planes.isEmpty()) convert();
        const int w=m_format.frameWidth(), h=m_format.frameHeight();
        const int cw=(w+1)/2, ch=(h+1)/2;
        uchar *base=reinterpret_cast<uchar*>(m_planes.data());
        result.planeCount=3;
        result.data[0]=base;                 result.bytesPerLine[0]=w;  result.dataSize[0]=w*h;
        result.data[1]=base+w*h;             result.bytesPerLine[1]=cw; result.dataSize[1]=cw*ch;
        result.data[2]=base+w*h+cw*ch;       result.bytesPerLine[2]=cw; result.dataSize[2]=cw*ch;
        return result;
    }
    void unmap() override {}
private:
    void convert() {
        const int w=m_format.frameWidth(), h=m_format.frameHeight();
        const int cw=(w+1)/2, ch=(h+1)/2;
        m_planes=QByteArray(qsizetype(w)*h+2*qsizetype(cw)*ch, Qt::Uninitialized);
        uchar *y=reinterpret_cast<uchar*>(m_planes.data()), *u=y+w*h, *v=u+cw*ch;
        bool converted=false;
        switch (m_source.pixelFormat()) {
        case QVideoFrameFormat::Format_Jpeg: converted=fromJpeg(y,u,v); break;
        case QVideoFrameFormat::Format_YUYV: converted=fromPacked(y,u,v,0,1,3); break;
        case QVideoFrameFormat::Format_UYVY: converted=fromPacked(y,u,v,1,0,2); break;
        default: converted=fromRgb(y,u,v); break;
        }
        // A corrupt webcam frame becomes neutral grey rather than failing the take.
        if(!converted) { std::memset(y, 128, size_t(w)*h); std::memset(u, 128, size_t(cw)*ch*2); }
        m_source=QVideoFrame();
    }
    static tjhandle decoder() {
        thread_local std::unique_ptr<void, int(*)(tjhandle)> handle(tj3Init(TJINIT_DECOMPRESS),
            [](tjhandle h) { tj3Destroy(h); return 0; });
        return handle.get();
    }
    bool fromJpeg(uchar *y, uchar *u, uchar *v) {
        const int w=m_format.frameWidth(), h=m_format.frameHeight();
        const int cw=(w+1)/2, ch=(h+1)/2;
        tjhandle tj=decoder();
        if(!tj || !m_source.map(QVideoFrame::ReadOnly)) return false;
        bool decoded=false;
        const auto *jpeg=m_source.bits(0);
        const size_t size=size_t(m_source.mappedBytes(0));
        if(tj3DecompressHeader(tj, jpeg, size)==0 &&
           tj3Get(tj, TJPARAM_JPEGWIDTH)==w && tj3Get(tj, TJPARAM_JPEGHEIGHT)==h) {
            const int subsamp=tj3Get(tj, TJPARAM_SUBSAMP);
            if(subsamp==TJSAMP_420) {
                unsigned char *planes[3]={y,u,v};
                int strides[3]={w,cw,cw};
                decoded=decodePlanes(tj, jpeg, size, planes, strides);
            } else if(subsamp==TJSAMP_GRAY) {
                unsigned char *planes[3]={y,nullptr,nullptr};
                int strides[3]={w,0,0};
                decoded=decodePlanes(tj, jpeg, size, planes, strides);
                std::memset(u, 128, size_t(cw)*ch*2);
            } else if(subsamp>=0) {
                const int sw=tj3YUVPlaneWidth(1, w, subsamp), sh=tj3YUVPlaneHeight(1, h, subsamp);
                QByteArray chroma(qsizetype(sw)*sh*2, Qt::Uninitialized);
                auto *su=reinterpret_cast<uchar*>(chroma.data()), *sv=su+sw*sh;
                unsigned char *planes[3]={y,su,sv};
                int strides[3]={w,sw,sw};
                decoded=decodePlanes(tj, jpeg, size, planes, strides);
                if(decoded) { resample(su, sw, sh, u, cw, ch); resample(sv, sw, sh, v, cw, ch); }
            }
        }
        m_source.unmap();
        return decoded;
    }
    static bool decodePlanes(tjhandle tj, const uchar *jpeg, size_t size, unsigned char **planes, int *strides) {
        // Warnings (such as a truncated scan) still yield a usable picture.
        return tj3DecompressToYUVPlanes8(tj, jpeg, size, planes, strides)==0 ||
               tj3GetErrorCode(tj)==TJERR_WARNING;
    }
    // Box-filter when the source has exactly twice the rows (4:2:2), else nearest.
    static void resample(const uchar *src, int sw, int sh, uchar *dst, int dw, int dh) {
        for(int row=0; row<dh; ++row) {
            uchar *out=dst+qsizetype(row)*dw;
            if(sw==dw && sh>=2*dh) {
                const uchar *a=src+qsizetype(2*row)*sw, *b=a+sw;
                for(int x=0; x<dw; ++x) out[x]=uchar((a[x]+b[x]+1)>>1);
            } else {
                const uchar *line=src+qsizetype(std::min(sh-1, int(qint64(row)*sh/dh)))*sw;
                for(int x=0; x<dw; ++x) out[x]=line[std::min(sw-1, int(qint64(x)*sw/dw))];
            }
        }
    }
    // Packed 4:2:2 (YUYV or UYVY): keep luma, average chroma over row pairs.
    bool fromPacked(uchar *y, uchar *u, uchar *v, int y0, int u0, int v0) {
        if(!m_source.map(QVideoFrame::ReadOnly)) return false;
        const int w=m_format.frameWidth(), h=m_format.frameHeight(), cw=(w+1)/2;
        const uchar *src=m_source.bits(0); const int stride=m_source.bytesPerLine(0);
        for(int row=0; row<h; ++row) {
            const uchar *line=src+qsizetype(row)*stride;
            uchar *out=y+qsizetype(row)*w;
            for(int x=0; x<w; ++x) out[x]=line[(x/2)*4+y0+(x&1)*2];
            if(row&1) continue;
            const uchar *next=row+1<h ? line+stride : line;
            uchar *ou=u+qsizetype(row/2)*cw, *ov=v+qsizetype(row/2)*cw;
            for(int x=0; x<cw; ++x) {
                ou[x]=uchar((line[x*4+u0]+next[x*4+u0]+1)>>1);
                ov[x]=uchar((line[x*4+v0]+next[x*4+v0]+1)>>1);
            }
        }
        m_source.unmap();
        return true;
    }
    // RGB of any layout, via QImage, to limited-range BT.601.
    bool fromRgb(uchar *y, uchar *u, uchar *v) {
        const QImage image=m_source.toImage().convertToFormat(QImage::Format_RGB32);
        const int w=m_format.frameWidth(), h=m_format.frameHeight(), cw=(w+1)/2;
        if(image.size()!=QSize(w,h)) return false;
        for(int row=0; row<h; ++row) {
            const auto *line=reinterpret_cast<const QRgb*>(image.constScanLine(row));
            uchar *out=y+qsizetype(row)*w;
            for(int x=0; x<w; ++x) {
                const int r=qRed(line[x]), g=qGreen(line[x]), b=qBlue(line[x]);
                out[x]=uchar(((66*r+129*g+25*b+128)>>8)+16);
            }
            if(row&1) continue;
            const auto *next=row+1<h ? reinterpret_cast<const QRgb*>(image.constScanLine(row+1)) : line;
            uchar *ou=u+qsizetype(row/2)*cw, *ov=v+qsizetype(row/2)*cw;
            for(int x=0; x<cw; ++x) {
                const int x1=std::min(2*x+1, w-1);
                const QRgb p[4]={line[2*x], line[x1], next[2*x], next[x1]};
                int r=0, g=0, b=0;
                for(const QRgb c:p) { r+=qRed(c); g+=qGreen(c); b+=qBlue(c); }
                r=(r+2)/4; g=(g+2)/4; b=(b+2)/4;
                ou[x]=uchar(((-38*r-74*g+112*b+128)>>8)+128);
                ov[x]=uchar(((112*r-94*g-18*b+128)>>8)+128);
            }
        }
        return true;
    }
    QVideoFrame m_source;
    QVideoFrameFormat m_format;
    QByteArray m_planes;
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
    const qint64 frameIndex = qRound64(position * m_fps / 1000000.0);
    const qint64 timestamp = qRound64(frameIndex * 1000000.0 / m_fps);
    if (timestamp <= m_lastVideo) return;
    m_tailFrame = frame;
    if (Yuv420FrameBuffer::converts(frame.pixelFormat()))
        frame = QVideoFrame(std::make_unique<Yuv420FrameBuffer>(frame));
    else
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
