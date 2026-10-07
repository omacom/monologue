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
