#include "pipewirecamera.h"
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/param/buffers.h>
#include <spa/buffer/meta.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <QMetaObject>
#include <cstring>
#include <mutex>

namespace {
const QByteArray prefix = "pipewire:";

void initPipeWire() {
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

QVideoFrameFormat::PixelFormat pixelFormat(uint32_t format) {
    // SPA names its packed RGB formats in byte order, as Qt does.
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRx: return QVideoFrameFormat::Format_BGRX8888;
    case SPA_VIDEO_FORMAT_BGRA: return QVideoFrameFormat::Format_BGRA8888;
    case SPA_VIDEO_FORMAT_RGBx: return QVideoFrameFormat::Format_RGBX8888;
    case SPA_VIDEO_FORMAT_RGBA: return QVideoFrameFormat::Format_RGBA8888;
    default: return QVideoFrameFormat::Format_Invalid;
    }
}

// One packed RGB format at a fixed size, or at any size when size is empty.
const spa_pod *formatParam(spa_pod_builder *b, QSize size) {
    spa_pod_frame f;
    spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
        0);
    if (size.isEmpty()) {
        const spa_rectangle def = SPA_RECTANGLE(1280, 720), min = SPA_RECTANGLE(1, 1), max = SPA_RECTANGLE(8192, 8192);
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def, &min, &max), 0);
    } else {
        const spa_rectangle fixed = SPA_RECTANGLE(uint32_t(size.width()), uint32_t(size.height()));
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&fixed), 0);
    }
    const spa_fraction rate = SPA_FRACTION(30, 1), minRate = SPA_FRACTION(0, 1), maxRate = SPA_FRACTION(120, 1);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&rate, &minRate, &maxRate), 0);
    return static_cast<const spa_pod *>(spa_pod_builder_pop(b, &f));
}
}

struct PipeWireCameraEvents {
    static void paramChanged(void *data, uint32_t id, const spa_pod *param) {
        auto *self = static_cast<PipeWireCamera *>(data);
        if (!param || id != SPA_PARAM_Format) return;
        spa_video_info_raw info{};
        uint32_t mediaType = 0, mediaSubtype = 0;
        if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 || mediaType != SPA_MEDIA_TYPE_video ||
            mediaSubtype != SPA_MEDIA_SUBTYPE_raw || spa_format_video_raw_parse(param, &info) < 0) return;
        self->m_pixelFormat = pixelFormat(info.format);
        self->m_size = QSize(int(info.size.width), int(info.size.height));
        // libcamera's nodes leave the rate open; they stream at the sensor's, which is 30 on webcams.
        self->m_fps = info.framerate.denom && info.framerate.num ? double(info.framerate.num) / info.framerate.denom : 30;
        if (self->m_pixelFormat == QVideoFrameFormat::Format_Invalid || self->m_size.isEmpty()) {
            pw_stream_set_error(self->m_stream, -EINVAL, "unsupported video format");
            return;
        }
        uint8_t buffer[512];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod *params[2];
        params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))));
        params[1] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
            SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header))));
        pw_stream_update_params(self->m_stream, params, 2);
        const QSize size = self->m_size;
        const double fps = self->m_fps;
        QMetaObject::invokeMethod(self, [self, size, fps] { emit self->formatChanged(size, fps); }, Qt::QueuedConnection);
    }

    static void stateChanged(void *data, pw_stream_state, pw_stream_state state, const char *error) {
        auto *self = static_cast<PipeWireCamera *>(data);
        QString message;
        if (state == PW_STREAM_STATE_ERROR)
            message = QString::fromUtf8(error ? error : "PipeWire stream error");
        else if (state == PW_STREAM_STATE_UNCONNECTED && self->m_streaming)
            message = PipeWireCamera::tr("The camera was disconnected.");
        if (state == PW_STREAM_STATE_STREAMING) self->m_streaming = true;
        if (!message.isEmpty())
            QMetaObject::invokeMethod(self, [self, message] { emit self->failed(message); }, Qt::QueuedConnection);
    }

    static void process(void *data) {
        auto *self = static_cast<PipeWireCamera *>(data);
        pw_buffer *buffer = pw_stream_dequeue_buffer(self->m_stream);
        if (!buffer) return;
        spa_buffer *spa = buffer->buffer;
        const spa_data &d = spa->datas[0];
        if (d.data && d.chunk && d.chunk->size && self->m_pixelFormat != QVideoFrameFormat::Format_Invalid) {
            const int width = self->m_size.width(), height = self->m_size.height();
            const int rowBytes = width * 4;
            const int stride = d.chunk->stride > 0 ? d.chunk->stride : rowBytes;
            const uint32_t offset = d.chunk->offset % d.maxsize;
            if (qint64(offset) + qint64(stride) * (height - 1) + rowBytes <= qint64(d.maxsize)) {
                QVideoFrame frame(QVideoFrameFormat(self->m_size, self->m_pixelFormat));
                if (frame.map(QVideoFrame::WriteOnly)) {
                    const auto *source = static_cast<const uchar *>(d.data) + offset;
                    uchar *target = frame.bits(0);
                    const int targetStride = frame.bytesPerLine(0);
                    for (int y = 0; y < height; ++y)
                        std::memcpy(target + qint64(y) * targetStride, source + qint64(y) * stride, size_t(rowBytes));
                    frame.unmap();
                    // The capture time, so jitter in PipeWire's delivery stays out of the take.
                    if (auto *header = static_cast<spa_meta_header *>(spa_buffer_find_meta_data(spa, SPA_META_Header, sizeof(spa_meta_header)));
                        header && header->pts > 0) {
                        frame.setStartTime(header->pts / 1000);
                        frame.setEndTime(header->pts / 1000 + qRound64(1000000.0 / self->m_fps));
                    }
                    QMetaObject::invokeMethod(self, [self, frame] { emit self->frameReady(frame); }, Qt::QueuedConnection);
                }
            }
        }
        pw_stream_queue_buffer(self->m_stream, buffer);
    }
};

static const pw_stream_events streamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = PipeWireCameraEvents::stateChanged;
    events.param_changed = PipeWireCameraEvents::paramChanged;
    events.process = PipeWireCameraEvents::process;
    return events;
}();

QList<PipeWireCamera::Source> PipeWireCamera::sources() {
    initPipeWire();
    struct Listing {
        pw_main_loop *loop = nullptr;
        int pending = 0;
        QList<Source> sources;
    } listing;
    listing.loop = pw_main_loop_new(nullptr);
    if (!listing.loop) return {};
    pw_context *context = pw_context_new(pw_main_loop_get_loop(listing.loop), nullptr, 0);
    pw_core *core = context ? pw_context_connect(context, nullptr, 0) : nullptr;
    if (!core) {
        if (context) pw_context_destroy(context);
        pw_main_loop_destroy(listing.loop);
        return {};
    }
    pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    spa_hook registryHook{}, coreHook{};
    static const pw_registry_events registryEvents = [] {
        pw_registry_events events{};
        events.version = PW_VERSION_REGISTRY_EVENTS;
        events.global = [](void *data, uint32_t, uint32_t, const char *type, uint32_t, const spa_dict *props) {
            auto *listing = static_cast<Listing *>(data);
            if (!props || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
            const char *mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
            const char *path = spa_dict_lookup(props, PW_KEY_OBJECT_PATH);
            const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
            // Qt already lists V4L2 cameras, so only libcamera's are added.
            if (!mediaClass || std::strcmp(mediaClass, "Video/Source") != 0 || !name ||
                !path || std::strncmp(path, "libcamera:", 10) != 0) return;
            const char *description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
            listing->sources.append({prefix + name, QString::fromUtf8(description ? description : name)});
        };
        return events;
    }();
    static const pw_core_events coreEvents = [] {
        pw_core_events events{};
        events.version = PW_VERSION_CORE_EVENTS;
        events.done = [](void *data, uint32_t id, int seq) {
            auto *listing = static_cast<Listing *>(data);
            if (id == PW_ID_CORE && seq == listing->pending) pw_main_loop_quit(listing->loop);
        };
        events.error = [](void *data, uint32_t id, int, int, const char *) {
            if (id == PW_ID_CORE) pw_main_loop_quit(static_cast<Listing *>(data)->loop);
        };
        return events;
    }();
    pw_registry_add_listener(registry, &registryHook, &registryEvents, &listing);
    pw_core_add_listener(core, &coreHook, &coreEvents, &listing);
    listing.pending = pw_core_sync(core, PW_ID_CORE, 0);
    pw_main_loop_run(listing.loop);
    spa_hook_remove(&coreHook);
    spa_hook_remove(&registryHook);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(listing.loop);
    return listing.sources;
}

bool PipeWireCamera::isSource(const QByteArray &id) { return id.startsWith(prefix); }

PipeWireCamera::PipeWireCamera(QObject *parent) : QObject(parent) { initPipeWire(); }

PipeWireCamera::~PipeWireCamera() { stop(); }

void PipeWireCamera::start(const QByteArray &id) {
    stop();
    m_loop = pw_thread_loop_new("monologue-camera", nullptr);
    if (!m_loop) { emit failed(tr("Could not start PipeWire.")); return; }
    pw_thread_loop_lock(m_loop);
    m_stream = pw_stream_new_simple(pw_thread_loop_get_loop(m_loop), "Monologue",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Camera",
                          PW_KEY_TARGET_OBJECT, id.mid(prefix.size()).constData(), nullptr),
        &streamEvents, this);
    bool connected = false;
    if (m_stream) {
        // Largest first: the stream takes the first of these the camera offers.
        uint8_t buffer[2048];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod *params[] = {formatParam(&b, {3840, 2160}), formatParam(&b, {2560, 1440}), formatParam(&b, {1920, 1080}),
                                   formatParam(&b, {1280, 720}), formatParam(&b, {})};
        connected = pw_stream_connect(m_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 5) == 0;
    }
    pw_thread_loop_unlock(m_loop);
    if (!connected || pw_thread_loop_start(m_loop) < 0) {
        stop();
        emit failed(tr("Could not connect to the camera through PipeWire."));
    }
}

void PipeWireCamera::stop() {
    if (!m_loop) return;
    pw_thread_loop_lock(m_loop);
    if (m_stream) { pw_stream_destroy(m_stream); m_stream = nullptr; }
    pw_thread_loop_unlock(m_loop);
    pw_thread_loop_stop(m_loop);
    pw_thread_loop_destroy(m_loop);
    m_loop = nullptr;
    m_streaming = false;
    m_pixelFormat = QVideoFrameFormat::Format_Invalid;
}
