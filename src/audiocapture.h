#pragma once
#include <QObject>
#include <QAudioFormat>
#include <QTimer>
#include <functional>
#include <algorithm>
#include <cmath>
#include <pulse/pulseaudio.h>

// Places capture buffers on one continuous timeline. A buffer's measured time
// (clock minus reported latency) is noisy: it jitters between buffers, wanders
// by a few milliseconds over tens of seconds, and takes about half a minute to
// settle after the stream opens. A writer that follows it trims or zero-pads
// at buffer boundaries, which is heard as crackle. The sample count is steady,
// so between takes the timeline follows the smoothed measurement, and from
// lock() on it only counts samples. An offset that outgrows the tolerance, from
// clock drift or a few lost samples, is then walked back one frame at a time.
class AudioTimeline {
public:
    struct Placement {
        qint64 capturedAt; // Time of the buffer's first sample.
        int slip;          // +1: repeat one frame, -1: drop one, to follow drift.
        bool resynced;     // The measurement left the timeline; it starts over here.
    };
    explicit AudioTimeline(int sampleRate = 48000) : m_rate(sampleRate) {}
    void lock() { m_locked = true; m_error = 0; m_following = false; }
    Placement place(qint64 measured, qint64 frames) {
        const double period = 1000000.0 / m_rate;
        const double error = measured - (m_origin + m_frames * period);
        Placement placed{0, 0, false};
        // One far reading is noise. Several in a row are a real discontinuity,
        // such as dropped samples or a stalled device.
        m_far = std::abs(error) > Resync ? m_far + 1 : 0;
        if (!m_started || m_far >= Confirm) {
            placed.resynced = m_started;
            m_started = true;
            m_origin = double(measured) - m_frames * period;
            m_buffers = 0; m_error = 0; m_far = 0; m_following = false;
        } else if (m_far) {
            // Not confirmed yet: place it by count and wait for the next reading.
        } else if (!m_locked) {
            // A plain mean at first, so one early reading does not set the origin.
            m_origin += error * std::max(frames * period / Smoothing, 1.0 / ++m_buffers);
        } else {
            m_error += (error - m_error) * std::min(1.0, frames * period / Smoothing);
            // Follow all the way back, so the offset does not rest at the tolerance.
            m_following = std::abs(m_error) > (m_following ? period : Tolerance);
            if (frames > 1 && m_following) {
                placed.slip = m_error > 0 ? 1 : -1;
                m_error -= placed.slip * period;
            }
        }
        placed.capturedAt = qRound64(m_origin + m_frames * period);
        m_frames += frames + placed.slip;
        return placed;
    }
private:
    // Microseconds. The tolerance sits above the settling and wander of the
    // reported latency, and below an offset anyone would notice. A restart
    // takes an offset no one should have to hear while it is walked back.
    static constexpr double Resync = 50000, Smoothing = 5000000, Tolerance = 20000;
    static constexpr int Confirm = 3;
    int m_rate;
    bool m_started = false, m_locked = false, m_following = false;
    qint64 m_frames = 0, m_buffers = 0;
    int m_far = 0;
    double m_origin = 0, m_error = 0;
};

// PulseAudio's capture latency includes unread/server/device buffering. Qt's
// QAudioSource only exposes samples and delivery time, losing that information.
// PipeWire's PulseAudio server implements the same timestamped capture API.
class AudioCapture : public QObject {
    Q_OBJECT
public:
    explicit AudioCapture(std::function<qint64()> clock, QObject *parent = nullptr);
    ~AudioCapture() override;
    void start(const QByteArray &device, const QAudioFormat &format);
    void stop();
    void readAvailable();
    // A take starts: from here on buffer times are contiguous by sample count.
    void lockTimeline() { m_timeline.lock(); }
    static qint64 captureTime(qint64 deliveredAt, qint64 latency, bool negative) {
        return deliveredAt + (negative ? latency : -latency);
    }
signals:
    void samples(const QByteArray &data, qint64 capturedAt);
    void failed(const QString &message);
private:
    void contextReady();
    void requestTiming();
    void fail(const QString &message);
    void pump();
    std::function<qint64()> m_clock;
    QTimer m_pump;
    QAudioFormat m_format;
    AudioTimeline m_timeline;
    QByteArray m_device;
    pa_mainloop *m_loop = nullptr;
    pa_context *m_context = nullptr;
    pa_stream *m_stream = nullptr;
    bool m_failed = false, m_reading = false, m_timingRequested = false, m_reportedTiming = false;
};
