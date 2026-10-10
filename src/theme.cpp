// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <cmath>

namespace {
// Qt probes every FFmpeg hardware device the first time multimedia is used.
// NVIDIA's VA-API driver segfaults in that probe (XDisplayString inside
// nvidia_drv_video.so), including when DISPLAY is unset or unauthorized.
// An explicit empty list skips the probe and keeps the software H.264 encoder.
// Naming CUDA instead makes Qt open h264_nvenc, which this driver rejects
// ("10 bit encode not supported"), and the take is stopped before libx264 starts.
// A value already set in the environment is left alone. This file is linked
// into the app and the tests, so the workaround is in place before main.
[[maybe_unused]] const int skipNvidiaVaapiProbe = [] {
    if (!QFileInfo::exists("/proc/driver/nvidia/version")) return 0;
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES", "");
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_DECODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", "");
    return 0;
}();
}

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl) : QObject(parent),
    m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);
    // Query the effective setting: themes and personal overrides can both set
    // rounding. Never execute or attempt to interpret Hyprland's Lua config.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE") ? QStandardPaths::findExecutable("hyprctl") : QString();
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this] { m_roundingTimeout.stop(); });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if(code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0) return;
        const int radius = value.toInt();
        if(radius != m_radius) { m_radius = radius; emit changed(); }
    });
    m_roundingPoll.setInterval(1000);
    connect(&m_roundingPoll, &QTimer::timeout, this, &Theme::refreshRounding);
    if(!m_hyprctl.isEmpty()) m_roundingPoll.start();
    reload();
}
Theme::~Theme() {
    if(m_roundingProcess.state() != QProcess::NotRunning) { m_roundingProcess.kill(); m_roundingProcess.waitForFinished(1000); }
}
void Theme::refreshRounding() {
    if(m_hyprctl.isEmpty() || m_roundingProcess.state() != QProcess::NotRunning) return;
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
}

QString Theme::readAccent(const QString &path) {
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QRegularExpression expression(R"re(^\s*accent\s*=\s*["'](#[0-9a-fA-F]{6})["']\s*(?:#.*)?$)re",
                                            QRegularExpression::MultilineOption);
        const auto match = expression.match(QString::fromUtf8(file.readAll()));
        if (match.hasMatch()) return match.captured(1);
    }
    return "#FFD60A";
}

QString Theme::contrastingColor(const QString &color) {
    const QColor c(color);
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    const double luminance = .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    return luminance > .179 ? "black" : "white";
}
QString Theme::foreground() const { return contrastingColor(m_accent); }

void Theme::reload() {
    refreshRounding();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty()) m_watcher.removePaths(paths);
    // Watch the parent too: the current or theme symlink can be replaced, and
    // installing Omarchy later should work without restarting this application.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/") ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory,
                           m_directory + "/theme", m_directory + "/theme/colors.toml"};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path)) m_watcher.addPath(path);
    const auto accent = readAccent(m_directory + "/theme/colors.toml");
    if (accent != m_accent) { m_accent = accent; emit changed(); }
}
