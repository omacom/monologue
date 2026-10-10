#include "windowlist.h"
#include <QCoreApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

std::optional<QList<window::Info>> window::clientsFromHyprctl(const QByteArray &json, qint64 ownPid) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray()) return std::nullopt;
    struct Raw { Info info; QString base, klass; };
    QList<Raw> rows;
    for (const auto &value : doc.array()) {
        if (!value.isObject()) continue;
        const auto object = value.toObject();
        // `hyprctl clients` already hides unmapped windows; `-a` would not.
        if (object.contains("mapped") && !object.value("mapped").toBool()) continue;
        if (static_cast<qint64>(object.value("pid").toDouble()) == ownPid) continue;
        const auto address = object.value("address").toString().trimmed();
        auto hex = address;
        if (hex.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) hex.remove(0, 2);
        bool ok = false;
        const quint64 full = hex.toULongLong(&ok, 16);
        if (!ok || full == 0) continue;
        Raw row;
        row.info.id = idPrefix + address;
        row.info.handle = static_cast<quint32>(full & 0xffffffffu);
        const auto title = object.value("title").toString().trimmed();
        row.klass = object.value("class").toString().trimmed();
        row.base = !title.isEmpty() ? title : (!row.klass.isEmpty() ? row.klass : QStringLiteral("Window"));
        row.info.label = row.base;
        row.info.title = row.base;
        row.info.windowClass = row.klass;
        const auto workspace = object.value("workspace");
        if (workspace.isObject()) row.info.workspace = workspace.toObject().value("name").toString().trimmed();
        else if (workspace.isString()) row.info.workspace = workspace.toString().trimmed();
        rows.append(row);
    }
    QHash<QString, int> bases, labels;
    for (const auto &row : rows) bases[row.base]++;
    for (auto &row : rows) {
        if (bases[row.base] < 2) continue;
        const auto extra = !row.klass.isEmpty() && row.klass != row.base ? row.klass : row.info.id.mid(idPrefix.size());
        row.info.label = row.base + QStringLiteral(" · ") + extra;
    }
    for (const auto &row : rows) labels[row.info.label]++;
    for (auto &row : rows)
        if (labels[row.info.label] > 1) row.info.label += QStringLiteral(" · ") + row.info.id.mid(idPrefix.size());
    QList<Info> result;
    for (const auto &row : rows) result.append(row.info);
    return result;
}

WindowList::WindowList(QObject *parent) : QObject(parent) {}
WindowList::~WindowList() {
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}
void WindowList::start() {
    if (m_started) return;
    m_started = true;
    if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE"))
        m_hyprctl = QStandardPaths::findExecutable("hyprctl");
    if (m_hyprctl.isEmpty()) { m_settled = true; return; }
    m_process = new QProcess(this);
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(1000);
    connect(m_timeout, &QTimer::timeout, m_process, &QProcess::kill);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        m_timeout->stop();
        apply(std::nullopt);
    });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_timeout->stop();
        const auto output = m_process->readAllStandardOutput();
        apply(code == 0 && status == QProcess::NormalExit ? window::clientsFromHyprctl(output, QCoreApplication::applicationPid()) : std::nullopt);
    });
    m_poll = new QTimer(this);
    m_poll->setInterval(1500);
    connect(m_poll, &QTimer::timeout, this, &WindowList::query);
    m_poll->start();
    query();
}
void WindowList::query() {
    if (m_hyprctl.isEmpty() || !m_process || m_process->state() != QProcess::NotRunning) return;
    m_process->start(m_hyprctl, {"-j", "clients"});
    m_timeout->start();
}
void WindowList::apply(const std::optional<QList<window::Info>> &parsed) {
    // A timed-out or unparseable reply is not "no windows"; keep the last good list.
    if (!parsed) {
        if (!m_settled) { m_settled = true; emit updated(); }
        return;
    }
    const bool changed = *parsed != m_windows;
    m_windows = *parsed;
    const bool first = !m_settled;
    m_settled = true;
    if (changed || first) emit updated();
}
