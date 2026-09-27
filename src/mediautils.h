#pragma once
#include <QAudioFormat>
#include <QCameraDevice>
#include <QJsonObject>
#include <QString>
#include <QImage>
#include <atomic>
#include "edit.h"

namespace media {
struct Format { QSize size; double minFps; double maxFps; int pixelFormat; };
int bestFormat(const QList<Format> &formats);
QCameraFormat bestCameraFormat(const QCameraDevice &device);
double targetFps(double minimum, double maximum);
double peak(const QByteArray &data, const QAudioFormat &format);
QString copyAtomically(const QString &source, const QString &destination);
QString normalizeMp4(const QString &path, double fps);
struct Probe { bool ok = false; QString error; QSize size; double duration = 0; bool audio = false; };
Probe probe(const QString &path);
// Re-encodes the kept ranges of a recording into one MP4, reporting progress on stdout.
QStringList exportArgs(const QString &source, const QString &destination, const QList<edit::Range> &kept, bool audio);
QImage thumbnail(const QString &path, double time, int height, const std::atomic<bool> *cancel);
}
