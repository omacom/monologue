#pragma once
#include <QAudioFormat>
#include <QCameraDevice>
#include <QJsonObject>
#include <QString>
#include <QImage>
#include <QVideoFrame>
#include <atomic>
#include "edit.h"

namespace media {
struct Format { QSize size; double minFps; double maxFps; int pixelFormat; };
int bestFormat(const QList<Format> &formats);
QCameraFormat bestCameraFormat(const QCameraDevice &device);
double targetFps(double minimum, double maximum);
// Decodes a camera JPEG to 8-bit 4:2:0 limited-range video, as delivered by
// webcams' MJPEG modes. Encoding Qt's RGB decode instead yields 10-bit 4:4:4
// H.264, too slow to sustain 4K and unplayable in most players.
QVideoFrame decodeJpeg(QByteArrayView jpeg);
double peak(const QByteArray &data, const QAudioFormat &format);
QString copyAtomically(const QString &source, const QString &destination);
QString normalizeMp4(const QString &path, double fps);
struct Probe { bool ok = false; QString error; QSize size; double duration = 0; bool audio = false; };
Probe probe(const QString &path);
// Re-encodes the kept ranges of a recording into one MP4, reporting progress on stdout.
QStringList exportArgs(const QString &source, const QString &destination, const QList<edit::Range> &kept, bool audio);
QImage thumbnail(const QString &path, double time, int height, const std::atomic<bool> *cancel);
}
