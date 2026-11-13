#include "audiocapture.h"
#include <limits>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(audioTiming, "monologue.audio", QtWarningMsg)

AudioCapture::AudioCapture(std::function<qint64()> clock, QObject *parent)
    : QObject(parent), m_clock(std::move(clock)) {
    m_pump.setTimerType(Qt::PreciseTimer);
    m_pump.setInterval(5);
    connect(&m_pump, &QTimer::timeout, this, &AudioCapture::pump);
}
AudioCapture::~AudioCapture() { stop(); }
void AudioCapture::start(const QByteArray &device, const QAudioFormat &format) {
    m_device = device;
    m_format = format;
    m_timeline = AudioTimeline(format.sampleRate());
    m_loop = pa_mainloop_new();
    if (!m_loop) { fail(tr("Could not create the audio capture loop.")); return; }
    m_context = pa_context_new(pa_mainloop_get_api(m_loop), "Monologue");
    if (!m_context) { fail(tr("Could not connect to the desktop audio server.")); return; }
    pa_context_set_state_callback(m_context, [](pa_context *context, void *opaque) {
        auto *self = static_cast<AudioCapture*>(opaque);
        switch (pa_context_get_state(context)) {
        case PA_CONTEXT_READY: self->contextReady(); break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED: self->fail(tr("The desktop audio server disconnected.")); break;
        default: break;
        }
    }, this);
    if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
        fail(tr("Could not connect to the PulseAudio-compatible desktop audio server.")); return;
    }
    m_pump.start();
}
void AudioCapture::contextReady() {
    pa_sample_spec spec{};
    spec.rate = m_format.sampleRate();
    spec.channels = m_format.channelCount();
    switch (m_format.sampleFormat()) {
    case QAudioFormat::UInt8: spec.format = PA_SAMPLE_U8; break;
    case QAudioFormat::Int16: spec.format = PA_SAMPLE_S16NE; break;
    case QAudioFormat::Int32: spec.format = PA_SAMPLE_S32NE; break;
    case QAudioFormat::Float: spec.format = PA_SAMPLE_FLOAT32NE; break;
    default: spec.format = PA_SAMPLE_INVALID; break;
    }
    if (!pa_sample_spec_valid(&spec)) { fail(tr("The microphone's sample format is not supported.")); return; }
    m_stream = pa_stream_new(m_context, "Microphone capture", &spec, nullptr);
    if (!m_stream) { fail(tr("Could not create the microphone stream.")); return; }
    pa_stream_set_state_callback(m_stream, [](pa_stream *stream, void *opaque) {
        auto *self = static_cast<AudioCapture*>(opaque);
        switch (pa_stream_get_state(stream)) {
        case PA_STREAM_READY: self->requestTiming(); break;
        case PA_STREAM_FAILED:
        case PA_STREAM_TERMINATED: self->fail(tr("The selected microphone disconnected or could not be opened.")); break;
        default: break;
        }
    }, this);
    pa_stream_set_read_callback(m_stream, [](pa_stream*, size_t, void *opaque) {
        static_cast<AudioCapture*>(opaque)->readAvailable();
    }, this);
    pa_buffer_attr buffers{};
    buffers.maxlength = m_format.bytesForDuration(2000000);
    buffers.fragsize = m_format.bytesForDuration(20000);
    buffers.tlength = buffers.prebuf = buffers.minreq = std::numeric_limits<uint32_t>::max();
    const auto flags = pa_stream_flags_t(PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_INTERPOLATE_TIMING |
                                        PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE);
    if (pa_stream_connect_record(m_stream, m_device.constData(), &buffers, flags) < 0)
        fail(tr("Could not open the selected microphone."));
}
void AudioCapture::requestTiming() {
    if (m_timingRequested || m_failed || !m_stream || pa_stream_get_state(m_stream) != PA_STREAM_READY) return;
    m_timingRequested = true;
    auto *operation = pa_stream_update_timing_info(m_stream, [](pa_stream*, int success, void *opaque) {
        auto *self = static_cast<AudioCapture*>(opaque);
        self->m_timingRequested = false;
        if (success) self->readAvailable();
    }, this);
    if (operation) pa_operation_unref(operation);
    else m_timingRequested = false;
}
void AudioCapture::pump() {
    if (m_failed || !m_loop) return;
    int result = 0;
    if (pa_mainloop_iterate(m_loop, 0, &result) < 0) { fail(tr("The audio capture loop stopped.")); return; }
    readAvailable();
}
void AudioCapture::readAvailable() {
    if (m_failed || m_reading || !m_stream || pa_stream_get_state(m_stream) != PA_STREAM_READY) return;
    m_reading = true;
    while (!m_failed) {
        const auto available = pa_stream_readable_size(m_stream);
        if (available == size_t(-1)) { fail(tr("Could not read the microphone.")); break; }
        if (!available) break;
        pa_usec_t latency = 0;
        int negative = 0;
        // This is the time of the oldest unread sample, not the time the Qt
        // event loop happens to deliver it. Same calculation as FFmpeg's Pulse
        // capture input, using our monotonic clock instead of wall-clock time.
        if (pa_stream_get_latency(m_stream, &latency, &negative) < 0) {
            requestTiming(); break; // Never invent a timestamp while timing is unavailable.
        }
        const qint64 capturedAt = captureTime(m_clock(), qint64(latency), negative);
        if (!m_reportedTiming) {
            qCDebug(audioTiming) << "Measured microphone capture latency (us):" << latency << "negative:" << negative;
            m_reportedTiming = true;
        }
        const void *data = nullptr;
        size_t bytes = 0;
        if (pa_stream_peek(m_stream, &data, &bytes) < 0) { fail(tr("Could not read microphone samples.")); break; }
        if (!bytes) break;
        if (bytes > size_t(m_format.bytesForDuration(2000000)) || bytes % m_format.bytesPerFrame()) {
            fail(tr("The microphone supplied an invalid audio buffer.")); break;
        }
        // PulseAudio holes are silence with a duration, not a clock reset.
        QByteArray pcm = data ? QByteArray(static_cast<const char*>(data), qsizetype(bytes))
                              : QByteArray(qsizetype(bytes), m_format.sampleFormat() == QAudioFormat::UInt8 ? char(128) : char(0));
        if (pa_stream_drop(m_stream) < 0) { fail(tr("Could not advance microphone capture.")); break; }
        const int frameBytes = m_format.bytesPerFrame();
        const auto placed = m_timeline.place(capturedAt, qint64(bytes) / frameBytes);
        if (placed.resynced) qCDebug(audioTiming) << "Microphone timeline restarted at" << capturedAt;
        if (placed.slip) qCDebug(audioTiming) << "Following microphone clock drift by" << placed.slip << "frame";
        if (placed.slip > 0) pcm.append(pcm.right(frameBytes));
        else if (placed.slip < 0) pcm.chop(frameBytes);
        emit samples(pcm, placed.capturedAt);
    }
    m_reading = false;
}
void AudioCapture::fail(const QString &message) {
    if (m_failed) return;
    m_failed = true;
    m_pump.stop();
    emit failed(message);
}
void AudioCapture::stop() {
    m_pump.stop();
    if (m_stream) {
        pa_stream_set_state_callback(m_stream, nullptr, nullptr);
        pa_stream_set_read_callback(m_stream, nullptr, nullptr);
        pa_stream_disconnect(m_stream);
        pa_stream_unref(m_stream);
        m_stream = nullptr;
    }
    if (m_context) {
        pa_context_set_state_callback(m_context, nullptr, nullptr);
        pa_context_disconnect(m_context);
        pa_context_unref(m_context);
        m_context = nullptr;
    }
    if (m_loop) { pa_mainloop_free(m_loop); m_loop = nullptr; }
}
