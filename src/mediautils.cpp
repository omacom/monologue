#include "mediautils.h"
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonArray>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstdio>
#include <memory>
#include <vector>
#include <turbojpeg.h>

double media::targetFps(double minimum, double maximum) { return std::clamp(30.0, minimum, maximum); }
int media::bestFormat(const QList<Format> &formats) {
    int best = -1;
    for (int i = 0; i < formats.size(); ++i) {
        const auto &f = formats[i];
        if (f.size.isEmpty() || f.maxFps <= 0 || f.minFps > f.maxFps) continue;
        if (best < 0) { best = i; continue; }
        const auto &b = formats[best];
        const qint64 area = qint64(f.size.width()) * f.size.height();
        const qint64 bestArea = qint64(b.size.width()) * b.size.height();
        const double rate = targetFps(f.minFps, f.maxFps), bestRate = targetFps(b.minFps, b.maxFps);
        if (area > bestArea || (area == bestArea &&
            (std::abs(rate - 30) < std::abs(bestRate - 30) ||
             (std::abs(rate - 30) == std::abs(bestRate - 30) &&
              (rate < bestRate || (rate == bestRate && f.maxFps < b.maxFps)))))) best = i;
    }
    return best;
}
QCameraFormat media::bestCameraFormat(const QCameraDevice &device) {
    const auto formats = device.videoFormats();
    QList<Format> choices;
    for (const auto &f : formats) choices.append({f.resolution(), f.minFrameRate(), f.maxFrameRate(), int(f.pixelFormat())});
    const int index = bestFormat(choices);
    return index < 0 ? QCameraFormat() : formats[index];
}
QVideoFrame media::decodeJpeg(QByteArrayView jpeg) {
    std::unique_ptr<void, decltype(&tj3Destroy)> tj(tj3Init(TJINIT_DECOMPRESS), tj3Destroy);
    const auto *data = reinterpret_cast<const unsigned char *>(jpeg.data());
    if (!tj || tj3DecompressHeader(tj.get(), data, jpeg.size()) != 0) return {};
    const int width = tj3Get(tj.get(), TJPARAM_JPEGWIDTH), height = tj3Get(tj.get(), TJPARAM_JPEGHEIGHT);
    const int sampling = tj3Get(tj.get(), TJPARAM_SUBSAMP), colorspace = tj3Get(tj.get(), TJPARAM_COLORSPACE);
    if (width <= 0 || height <= 0 || width % 2 || height % 2 || sampling == TJSAMP_UNKNOWN ||
        (colorspace != TJCS_YCbCr && colorspace != TJCS_GRAY)) return {};
    // Decode to the JPEG's own planes, skipping libjpeg's RGB conversion.
    const int planes = sampling == TJSAMP_GRAY ? 1 : 3;
    std::vector<unsigned char> source[3];
    unsigned char *destinations[3] = {};
    int strides[3] = {}, heights[3] = {};
    for (int i = 0; i < planes; ++i) {
        strides[i] = tj3YUVPlaneWidth(i, width, sampling);
        heights[i] = tj3YUVPlaneHeight(i, height, sampling);
        source[i].resize(size_t(strides[i]) * heights[i]);
        destinations[i] = source[i].data();
    }
    if (tj3DecompressToYUVPlanes8(tj.get(), data, jpeg.size(), destinations, strides) != 0) return {};

    QVideoFrameFormat format(QSize(width, height), QVideoFrameFormat::Format_YUV420P);
    format.setColorSpace(QVideoFrameFormat::ColorSpace_BT601);
    format.setColorRange(QVideoFrameFormat::ColorRange_Video);
    QVideoFrame frame(format);
    if (!frame.map(QVideoFrame::WriteOnly)) return {};
    // JPEG is full range; H.264 players expect video range.
    unsigned char luma[256], chroma[256];
    for (int v = 0; v < 256; ++v) { luma[v] = 16 + (v * 219 + 127) / 255; chroma[v] = 16 + (v * 224 + 127) / 255; }
    for (int y = 0; y < height; ++y) {
        const unsigned char *in = source[0].data() + size_t(y) * strides[0];
        unsigned char *out = frame.bits(0) + qsizetype(y) * frame.bytesPerLine(0);
        for (int x = 0; x < width; ++x) out[x] = luma[in[x]];
    }
    // Box-filter each chroma plane, whatever its sampling, onto the 4:2:0 grid.
    const int outWidth = width / 2, outHeight = height / 2;
    std::vector<int> columns(outWidth + 1);
    for (int i = 1; i < 3; ++i) {
        unsigned char *plane = frame.bits(i);
        const int bytesPerLine = frame.bytesPerLine(i);
        if (planes == 1) {
            for (int y = 0; y < outHeight; ++y) std::memset(plane + qsizetype(y) * bytesPerLine, 128, outWidth);
            continue;
        }
        for (int x = 0; x <= outWidth; ++x) columns[x] = qint64(x) * strides[i] / outWidth;
        for (int y = 0; y < outHeight; ++y) {
            const int top = qint64(y) * heights[i] / outHeight;
            const int bottom = std::max(top + 1, int(qint64(y + 1) * heights[i] / outHeight));
            unsigned char *out = plane + qsizetype(y) * bytesPerLine;
            for (int x = 0; x < outWidth; ++x) {
                const int left = columns[x], right = std::max(left + 1, columns[x + 1]);
                int sum = 0;
                for (int row = top; row < bottom; ++row) {
                    const unsigned char *in = source[i].data() + size_t(row) * strides[i];
                    for (int column = left; column < right; ++column) sum += in[column];
                }
                const int count = (bottom - top) * (right - left);
                out[x] = chroma[(sum + count / 2) / count];
            }
        }
    }
    frame.unmap();
    return frame;
}
double media::peak(const QByteArray &data, const QAudioFormat &format) {
    double result = 0;
    const int bytes = format.bytesPerSample();
    if (!bytes) return 0;
    for (qsizetype offset = 0; offset + bytes <= data.size(); offset += bytes) {
        double sample = 0;
        switch (format.sampleFormat()) {
        case QAudioFormat::UInt8: sample = (quint8(data[offset]) - 128) / 128.0; break;
        case QAudioFormat::Int16: { qint16 v; std::memcpy(&v, data.constData() + offset, 2); sample = v / 32768.0; break; }
        case QAudioFormat::Int32: { qint32 v; std::memcpy(&v, data.constData() + offset, 4); sample = v / 2147483648.0; break; }
        case QAudioFormat::Float: { float v; std::memcpy(&v, data.constData() + offset, 4); sample = std::isfinite(v) ? v : 0; break; }
        default: break;
        }
        result = std::max(result, std::abs(sample));
    }
    return std::min(result, 1.0);
}
QString media::copyAtomically(const QString &source, const QString &destination) {
    if (QFileInfo(source).canonicalFilePath() == QFileInfo(destination).canonicalFilePath())
        return "Choose a destination outside the retained recording itself.";
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) return input.errorString();
    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) return output.errorString();
    while (!input.atEnd()) {
        const auto block = input.read(1024 * 1024);
        if (block.isEmpty() && input.error() != QFile::NoError) return input.errorString();
        if (output.write(block) != block.size()) return output.errorString();
    }
    if (!output.commit()) return output.errorString();
    return {};
}
QString media::normalizeMp4(const QString &path, double fps) {
    // Qt 6.11 estimates packet duration from successive PTS, which is wrong for
    // reordered frames and for the last frame of a jittery webcam stream.
    // Remux (no re-encoding), preserving PTS/DTS but assigning one nominal frame
    // duration. Gaps between presentation timestamps still hold the last image.
    const auto temporary=path+".finalizing.mp4";
    QProcess process;
    process.start("ffmpeg", {"-nostdin", "-v", "error", "-y", "-i", path,
                            "-map", "0", "-c", "copy", "-bsf:v",
                            "setts=pts=PTS:dts=DTS:duration=1/("+QString::number(fps,'g',12)+"*TB)",
                            "-movflags", "+faststart", temporary});
    if(!process.waitForFinished(120000)) {
        process.kill(); process.waitForFinished();
        return "Finalizing the recording timed out.";
    }
    if(process.exitStatus()!=QProcess::NormalExit || process.exitCode()!=0)
        return "Could not finalize the MP4: "+QString::fromUtf8(process.readAllStandardError()).left(500);
    if(std::rename(QFile::encodeName(temporary).constData(),QFile::encodeName(path).constData())!=0)
        return "Could not replace the finalized clip.";
    return {};
}
media::Probe media::probe(const QString &path) {
    Probe result;
    QProcess process;
    process.start("ffprobe", {"-v", "error", "-show_streams", "-show_format", "-of", "json", path});
    if (!process.waitForFinished(30000)) {
        process.kill(); process.waitForFinished();
        result.error = "Could not inspect the clip with ffprobe."; return result;
    }
    const auto doc = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    for (const auto &entry : doc["streams"].toArray()) {
        const auto stream = entry.toObject();
        if (stream["codec_type"] == "video" && stream["codec_name"] == "h264")
            result.size = QSize(stream["width"].toInt(), stream["height"].toInt());
        if (stream["codec_type"] == "audio" && stream["codec_name"] == "aac") result.audio = true;
    }
    result.duration = doc["format"].toObject()["duration"].toString().toDouble();
    result.ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 &&
                !result.size.isEmpty() && result.duration > 0;
    if (!result.ok) result.error = "The recording could not be finalized as a playable H.264 MP4.";
    return result;
}
QStringList media::exportArgs(const QString &source, const QString &destination, const QList<edit::Range> &kept, bool audio) {
    QStringList args{"-nostdin", "-v", "error", "-y", "-progress", "pipe:1", "-nostats"};
    QString graph;
    // One seeking input per kept range decodes only what survives the cut.
    for (int i = 0; i < kept.size(); ++i) {
        args << "-ss" << QString::number(kept[i].start, 'f', 3) << "-t" << QString::number(kept[i].length(), 'f', 3) << "-i" << source;
        graph += QString("[%1:v:0]").arg(i) + (audio ? QString("[%1:a:0]").arg(i) : QString());
    }
    graph += QString("concat=n=%1:v=1:a=%2[v]").arg(kept.size()).arg(audio ? 1 : 0) + (audio ? "[a]" : "");
    args << "-filter_complex" << graph << "-map" << "[v]";
    if (audio) args << "-map" << "[a]" << "-c:a" << "aac" << "-b:a" << "192k";
    args << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "18" << "-pix_fmt" << "yuv420p"
         << "-movflags" << "+faststart" << "-f" << "mp4" << destination;
    return args;
}
QImage media::thumbnail(const QString &path, double time, int height, const std::atomic<bool> *cancel) {
    QProcess process;
    process.start("ffmpeg", {"-nostdin", "-loglevel", "error", "-ss", QString::number(std::max(time, 0.0), 'f', 3), "-i", path,
                             "-frames:v", "1", "-vf", QString("scale=-2:%1").arg(height), "-f", "image2pipe", "-vcodec", "mjpeg", "pipe:1"});
    // Poll, so a cancelled strip kills ffmpeg promptly instead of blocking on it.
    while (!process.waitForFinished(50)) {
        if (process.state() == QProcess::NotRunning) break;
        if (cancel && cancel->load(std::memory_order_relaxed)) { process.kill(); process.waitForFinished(); return {}; }
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) return {};
    QImage image;
    image.loadFromData(process.readAllStandardOutput(), "JPEG");
    return image;
}
