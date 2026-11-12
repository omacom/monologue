#pragma once
#include <QObject>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QProcess>

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString foreground READ foreground NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    Q_PROPERTY(bool light READ light NOTIFY changed)
public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {});
    ~Theme() override;
    int radius() const { return m_radius; }
    QString accent() const { return m_accent; }
    bool light() const { return m_light; }
    QString foreground() const;
    static QString readAccent(const QString &path);
    static bool readLight(const QString &path);
    static QString contrastingColor(const QString &color);
signals:
    void changed();
private:
    void reload();
    void refreshRounding();
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
    QString m_directory;
    QString m_accent = "#FFD60A";
    bool m_light = false;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
