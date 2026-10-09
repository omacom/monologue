#include "encoder.h"
#include <QImage>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {
QString failure(const char *what, int code) {
    char reason[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, reason, sizeof reason);
    return QString("%1: %2").arg(what, reason);
}
AVPixelFormat pixelFormat(QVideoFrameFormat::PixelFormat format) {
    switch (format) {
    case QVideoFrameFormat::Format_YUV420P: return AV_PIX_FMT_YUV420P;
    case QVideoFrameFormat::Format_YUV422P: return AV_PIX_FMT_YUV422P;
    case QVideoFrameFormat::Format_NV12: return AV_PIX_FMT_NV12;
    case QVideoFrameFormat::Format_YUYV: return AV_PIX_FMT_YUYV422;
    case QVideoFrameFormat::Format_UYVY: return AV_PIX_FMT_UYVY422;
    case QVideoFrameFormat::Format_BGRA8888: return AV_PIX_FMT_BGRA;
    case QVideoFrameFormat::Format_BGRX8888: return AV_PIX_FMT_BGR0;
    case QVideoFrameFormat::Format_RGBA8888: return AV_PIX_FMT_RGBA;
    case QVideoFrameFormat::Format_RGBX8888: return AV_PIX_FMT_RGB0;
    case QVideoFrameFormat::Format_ARGB8888: return AV_PIX_FMT_ARGB;
    case QVideoFrameFormat::Format_XRGB8888: return AV_PIX_FMT_0RGB;
    case QVideoFrameFormat::Format_ABGR8888: return AV_PIX_FMT_ABGR;
    case QVideoFrameFormat::Format_XBGR8888: return AV_PIX_FMT_0BGR;
    default: return AV_PIX_FMT_NONE;
    }
}
AVSampleFormat sampleFormat(QAudioFormat::SampleFormat format) {
    switch (format) {
    case QAudioFormat::UInt8: return AV_SAMPLE_FMT_U8;
    case QAudioFormat::Int16: return AV_SAMPLE_FMT_S16;
    case QAudioFormat::Int32: return AV_SAMPLE_FMT_S32;
    case QAudioFormat::Float: return AV_SAMPLE_FMT_FLT;
    default: return AV_SAMPLE_FMT_NONE;
    }
}
}

Encoder::~Encoder() {
    {
        std::lock_guard lock(m_mutex);
        m_finishing = m_discard = true;
        m_done = nullptr; m_failed = nullptr;
    }
    m_wake.notify_one();
    if (m_thread.joinable()) m_thread.join();
    release();
}
bool Encoder::supported(bool audio) {
    return av_guess_format("mp4", nullptr, nullptr) &&
           (avcodec_find_encoder_by_name("h264_vaapi") || avcodec_find_encoder_by_name("libx264")) &&
           (!audio || avcodec_find_encoder(AV_CODEC_ID_AAC));
}
QString Encoder::open(const Settings &settings, std::function<void(const QString &)> failed) {
    m_failed = std::move(failed);
    m_audioFormat = settings.audio;
    const auto path = settings.path.toUtf8();
    int result = avformat_alloc_output_context2(&m_format, nullptr, "mp4", path.constData());
    if (result < 0) return failure("Could not create the MP4", result);
    auto error = openVideo(settings, true);
    if (!error.isEmpty()) error = openVideo(settings, false);
    if (error.isEmpty() && settings.audio.isValid()) error = openAudio(settings.audio);
    if (error.isEmpty() && (result = avio_open(&m_format->pb, path.constData(), AVIO_FLAG_WRITE)) < 0)
        error = failure("Could not open the recording file", result);
    if (error.isEmpty() && (result = avformat_write_header(m_format, nullptr)) < 0)
        error = failure("Could not start the MP4", result);
    if (!error.isEmpty()) { release(); return error; }
    m_headerWritten = true;
    m_thread = std::thread(&Encoder::run, this);
    return {};
}
QString Encoder::openVideo(const Settings &settings, bool hardware) {
    const AVCodec *codec = avcodec_find_encoder_by_name(hardware ? "h264_vaapi" : "libx264");
    if (!codec) return "No H.264 encoder is available.";
    int result = 0;
    if (hardware && (result = av_hwdevice_ctx_create(&m_device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0)) < 0)
        return failure("No VAAPI device", result);
    const AVRational rate = av_d2q(settings.fps, 1001000);
    m_video = avcodec_alloc_context3(codec);
    m_video->width = settings.size.width();
    m_video->height = settings.size.height();
    m_video->framerate = rate;
    m_video->time_base = av_inv_q(rate);
    // Keyframes every two seconds keep the editor's seeking quick.
    m_video->gop_size = std::max(1, int(settings.fps * 2));
    m_video->color_range = AVCOL_RANGE_MPEG;
    m_video->colorspace = AVCOL_SPC_BT470BG;
    if (m_format->oformat->flags & AVFMT_GLOBALHEADER) m_video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary *options = nullptr;
    const AVPixelFormat software = hardware ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    if (hardware) {
        AVBufferRef *frames = av_hwframe_ctx_alloc(m_device);
        auto *context = reinterpret_cast<AVHWFramesContext *>(frames->data);
        context->format = AV_PIX_FMT_VAAPI;
        context->sw_format = software;
        context->width = m_video->width;
        context->height = m_video->height;
        context->initial_pool_size = 20;
        if ((result = av_hwframe_ctx_init(frames)) < 0) {
            av_buffer_unref(&frames);
            avcodec_free_context(&m_video); av_buffer_unref(&m_device);
            return failure("Could not allocate VAAPI frames", result);
        }
        m_video->hw_frames_ctx = frames;
        m_video->pix_fmt = AV_PIX_FMT_VAAPI;
        m_video->profile = AV_PROFILE_H264_HIGH;
        // Constant QP 20 is close to the CRF 19 libx264 quality Qt used.
        m_video->global_quality = 20;
        av_dict_set(&options, "rc_mode", "CQP", 0);
    } else {
        m_video->pix_fmt = software;
        av_dict_set(&options, "preset", "veryfast", 0);
        av_dict_set(&options, "crf", "19", 0);
    }
    result = avcodec_open2(m_video, codec, &options);
    av_dict_free(&options);
    if (result < 0) {
        avcodec_free_context(&m_video); av_buffer_unref(&m_device);
        return failure("Could not open the H.264 encoder", result);
    }
    m_software = av_frame_alloc();
    m_software->format = software;
    m_software->width = m_video->width;
    m_software->height = m_video->height;
    if ((result = av_frame_get_buffer(m_software, 0)) < 0) return failure("Could not allocate a video frame", result);
    m_videoStream = avformat_new_stream(m_format, nullptr);
    m_videoStream->time_base = m_video->time_base;
    m_videoStream->avg_frame_rate = rate;
    avcodec_parameters_from_context(m_videoStream->codecpar, m_video);
    m_videoEncoderName = codec->name;
    return {};
}
QString Encoder::openAudio(const QAudioFormat &format) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    const AVSampleFormat input = sampleFormat(format.sampleFormat());
    if (!codec || input == AV_SAMPLE_FMT_NONE) return "No AAC encoder is available for this microphone format.";
    m_audio = avcodec_alloc_context3(codec);
    m_audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
    m_audio->sample_rate = format.sampleRate();
    av_channel_layout_default(&m_audio->ch_layout, format.channelCount());
    m_audio->bit_rate = 192000;
    m_audio->time_base = {1, format.sampleRate()};
    if (m_format->oformat->flags & AVFMT_GLOBALHEADER) m_audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int result = avcodec_open2(m_audio, codec, nullptr);
    if (result < 0) return failure("Could not open the AAC encoder", result);
    if ((result = swr_alloc_set_opts2(&m_resample, &m_audio->ch_layout, AV_SAMPLE_FMT_FLTP, m_audio->sample_rate,
                                      &m_audio->ch_layout, input, m_audio->sample_rate, 0, nullptr)) < 0 ||
        (result = swr_init(m_resample)) < 0)
        return failure("Could not convert the microphone audio", result);
    m_fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, format.channelCount(), std::max(1, m_audio->frame_size));
    m_audioStream = avformat_new_stream(m_format, nullptr);
    m_audioStream->time_base = m_audio->time_base;
    avcodec_parameters_from_context(m_audioStream->codecpar, m_audio);
    return {};
}
void Encoder::video(const QVideoFrame &frame) {
    {
        std::lock_guard lock(m_mutex);
        if (m_finishing) return;
        m_jobs.push_back({frame, {}});
        ++m_queuedFrames;
    }
    m_wake.notify_one();
}
void Encoder::audio(const QByteArray &pcm) {
    {
        std::lock_guard lock(m_mutex);
        if (m_finishing || !m_audio) return;
        m_jobs.push_back({{}, pcm});
        m_queuedAudioUs += m_audioFormat.durationForBytes(pcm.size());
    }
    m_wake.notify_one();
}
void Encoder::finish(bool discard, std::function<void(const QString &)> done) {
    {
        std::lock_guard lock(m_mutex);
        m_finishing = true;
        m_discard = m_discard || discard;
        m_done = std::move(done);
    }
    m_wake.notify_one();
}
void Encoder::run() {
    QString error;
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return !m_jobs.empty() || m_finishing; });
            if (m_discard) { m_jobs.clear(); m_queuedFrames = 0; m_queuedAudioUs = 0; }
            if (m_jobs.empty()) break;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        if (error.isEmpty()) {
            error = job.frame.isValid() ? encodeVideo(job.frame) : encodeAudio(job.pcm, false);
            if (!error.isEmpty()) {
                std::lock_guard lock(m_mutex);
                if (m_failed) m_failed(error);
            }
        }
        if (job.frame.isValid()) --m_queuedFrames;
        else m_queuedAudioUs -= m_audioFormat.durationForBytes(job.pcm.size());
    }
    if (error.isEmpty() && m_audio) error = encodeAudio({}, true);
    const auto closing = close();
    if (error.isEmpty()) error = closing;
    // Hold the lock so the owner cannot be destroyed while it is notified.
    std::lock_guard lock(m_mutex);
    if (m_done) m_done(error);
}
QString Encoder::encodeVideo(const QVideoFrame &source) {
    QVideoFrame frame = source;
    AVPixelFormat format = pixelFormat(frame.pixelFormat());
    const uint8_t *planes[4] = {};
    int strides[4] = {};
    QImage image;
    if (format == AV_PIX_FMT_NONE) {
        image = frame.toImage().convertToFormat(QImage::Format_RGB32);
        if (image.isNull()) return "A camera frame could not be converted for encoding.";
        format = AV_PIX_FMT_BGR0;
        planes[0] = image.constBits(); strides[0] = int(image.bytesPerLine());
    } else {
        if (!frame.map(QVideoFrame::ReadOnly)) return "A camera frame could not be read for encoding.";
        for (int i = 0; i < frame.planeCount(); ++i) { planes[i] = frame.bits(i); strides[i] = frame.bytesPerLine(i); }
    }
    const QSize size = image.isNull() ? frame.size() : image.size();
    m_scale = sws_getCachedContext(m_scale, size.width(), size.height(), format,
                                   m_video->width, m_video->height, AVPixelFormat(m_software->format),
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
    int result = m_scale ? av_frame_make_writable(m_software) : AVERROR(EINVAL);
    if (result >= 0) sws_scale(m_scale, planes, strides, 0, size.height(), m_software->data, m_software->linesize);
    if (image.isNull()) frame.unmap();
    if (result < 0) return failure("Could not convert a camera frame", result);
    // Frame times are already quantized to the output rate.
    m_software->pts = av_rescale_q_rnd(frame.startTime(), {1, 1000000}, m_video->time_base,
                                       AVRounding(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    if (!m_device) return send(m_video, m_videoStream, m_software);
    AVFrame *upload = av_frame_alloc();
    if ((result = av_hwframe_get_buffer(m_video->hw_frames_ctx, upload, 0)) < 0 ||
        (result = av_hwframe_transfer_data(upload, m_software, 0)) < 0) {
        av_frame_free(&upload);
        return failure("Could not upload a frame to the GPU", result);
    }
    upload->pts = m_software->pts;
    const auto error = send(m_video, m_videoStream, upload);
    av_frame_free(&upload);
    return error;
}
QString Encoder::encodeAudio(const QByteArray &pcm, bool flush) {
    const int channels = m_audio->ch_layout.nb_channels;
    if (!pcm.isEmpty()) {
        const int samples = m_audioFormat.framesForBytes(pcm.size());
        uint8_t **converted = nullptr;
        int result = av_samples_alloc_array_and_samples(&converted, nullptr, channels, samples, AV_SAMPLE_FMT_FLTP, 0);
        if (result < 0) return failure("Could not convert microphone audio", result);
        const uint8_t *input[1] = {reinterpret_cast<const uint8_t *>(pcm.constData())};
        result = swr_convert(m_resample, converted, samples, input, samples);
        if (result > 0) result = av_audio_fifo_write(m_fifo, reinterpret_cast<void **>(converted), result);
        av_freep(&converted[0]); av_freep(&converted);
        if (result < 0) return failure("Could not convert microphone audio", result);
    }
    const int frameSize = m_audio->frame_size > 0 ? m_audio->frame_size : 1024;
    while (av_audio_fifo_size(m_fifo) >= frameSize || (flush && av_audio_fifo_size(m_fifo) > 0)) {
        const int available = std::min(frameSize, av_audio_fifo_size(m_fifo));
        const bool padded = available < frameSize && !(m_audio->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME);
        AVFrame *frame = av_frame_alloc();
        frame->nb_samples = padded ? frameSize : available;
        frame->format = AV_SAMPLE_FMT_FLTP;
        frame->sample_rate = m_audio->sample_rate;
        av_channel_layout_copy(&frame->ch_layout, &m_audio->ch_layout);
        int result = av_frame_get_buffer(frame, 0);
        if (result >= 0) {
            if (padded) av_samples_set_silence(frame->data, 0, frameSize, channels, AV_SAMPLE_FMT_FLTP);
            result = av_audio_fifo_read(m_fifo, reinterpret_cast<void **>(frame->data), available);
        }
        if (result < 0) { av_frame_free(&frame); return failure("Could not buffer microphone audio", result); }
        frame->pts = m_audioSamples;
        m_audioSamples += frame->nb_samples;
        const auto error = send(m_audio, m_audioStream, frame);
        av_frame_free(&frame);
        if (!error.isEmpty()) return error;
    }
    return {};
}
QString Encoder::send(AVCodecContext *codec, AVStream *stream, AVFrame *frame) {
    int result = avcodec_send_frame(codec, frame);
    if (result < 0) return failure("Encoding failed", result);
    AVPacket *packet = av_packet_alloc();
    while ((result = avcodec_receive_packet(codec, packet)) >= 0) {
        av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
        packet->stream_index = stream->index;
        if ((result = av_interleaved_write_frame(m_format, packet)) < 0) break;
    }
    av_packet_free(&packet);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return {};
    return failure("Could not write the recording", result);
}
QString Encoder::close() {
    QString error;
    if (m_video) error = send(m_video, m_videoStream, nullptr);
    if (m_audio) { const auto audio = send(m_audio, m_audioStream, nullptr); if (error.isEmpty()) error = audio; }
    if (m_headerWritten) {
        const int result = av_write_trailer(m_format);
        if (result < 0 && error.isEmpty()) error = failure("Could not finish the MP4", result);
        m_headerWritten = false;
    }
    release();
    return error;
}
void Encoder::release() {
    avcodec_free_context(&m_video);
    avcodec_free_context(&m_audio);
    av_buffer_unref(&m_device);
    av_frame_free(&m_software);
    sws_freeContext(m_scale); m_scale = nullptr;
    swr_free(&m_resample);
    if (m_fifo) { av_audio_fifo_free(m_fifo); m_fifo = nullptr; }
    if (m_format) {
        if (m_format->pb) avio_closep(&m_format->pb);
        avformat_free_context(m_format);
        m_format = nullptr;
    }
    m_videoStream = m_audioStream = nullptr;
}
