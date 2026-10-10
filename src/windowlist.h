#pragma once
#include <QObject>
#include <QList>
#include <QString>
#include <optional>

class QProcess;
class QTimer;

// Hyprland windows listed by `hyprctl -j clients`. The capture handle is the
// low 32 bits of the address string: the toplevel-export protocol takes that
// uint32, not the text form (d161e7b0 is 3512854448).
namespace window {
inline const QString idPrefix = QStringLiteral("window:");
inline bool isWindow(const QString &id) { return id.startsWith(idPrefix); }
struct Info {
    QString id;
    QString label;
    QString title;
    QString windowClass;
    QString workspace;
    quint32 handle = 0;
    bool operator==(const Info &other) const {
        return id == other.id && label == other.label && title == other.title && windowClass == other.windowClass
            && workspace == other.workspace && handle == other.handle;
    }
};
// nullopt when the payload is not a client array, so a failed query can keep
// the previous list. An empty array is a successful empty list.
std::optional<QList<Info>> clientsFromHyprctl(const QByteArray &json, qint64 ownPid);
}

// Polls hyprctl while a Hyprland session is present. No compositor and no
// process are touched until start(), and start() does nothing without
// HYPRLAND_INSTANCE_SIGNATURE, so offscreen tests never open a socket.
class WindowList : public QObject {
    Q_OBJECT
public:
    explicit WindowList(QObject *parent = nullptr);
    ~WindowList() override;
    void start();
    bool settled() const { return m_settled; }
    QList<window::Info> windows() const { return m_windows; }
signals:
    void updated();
private:
    void query();
    void apply(const std::optional<QList<window::Info>> &parsed);
    QString m_hyprctl;
    QList<window::Info> m_windows;
    QProcess *m_process = nullptr;
    QTimer *m_poll = nullptr;
    QTimer *m_timeout = nullptr;
    bool m_started = false, m_settled = false;
};
