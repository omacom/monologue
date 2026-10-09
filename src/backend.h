#pragma once
#include <QObject>
#include <QMediaDevices>
#include <QMediaCaptureSession>
#include <QCamera>
#include "audiocapture.h"
#include <QVideoSink>
#include <QElapsedTimer>
#include <QSettings>
#include <QPointer>
#include <QVariantList>
#include <QTimer>
#include <QProcess>
#include <QLockFile>
#include <memory>
#include "writer.h"
#include "edit.h"
#include "thumbnails.h"
#include "portalfilepicker.h"

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString formatLabel READ formatLabel NOTIFY changed)
    Q_PROPERTY(QVariantList cameras READ cameras NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList cameraFormats READ cameraFormats NOTIFY devicesChanged)
    Q_PROPERTY(int cameraFormatIndex READ cameraFormatIndex NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList microphones READ microphones NOTIFY devicesChanged)
    Q_PROPERTY(int cameraIndex READ cameraIndex NOTIFY devicesChanged)
    Q_PROPERTY(int microphoneIndex READ microphoneIndex NOTIFY devicesChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool audioEnabled READ audioEnabled NOTIFY changed)
    Q_PROPERTY(bool takeActive READ takeActive NOTIFY changed)
    Q_PROPERTY(bool dialogOpen READ dialogOpen NOTIFY changed)
    Q_PROPERTY(double duration READ duration NOTIFY changed)
    Q_PROPERTY(double level READ level NOTIFY meterChanged)
    Q_PROPERTY(double peakLevel READ peakLevel NOTIFY meterChanged)
    Q_PROPERTY(bool clipping READ clipping NOTIFY meterChanged)
    Q_PROPERTY(QString meterText READ meterText NOTIFY meterChanged)
    Q_PROPERTY(QUrl clip READ clip NOTIFY changed)
    Q_PROPERTY(double saveProgress READ saveProgress NOTIFY changed)
    Q_PROPERTY(QVariantList clips READ clips NOTIFY editChanged)
    Q_PROPERTY(QVariantList pauses READ pauses NOTIFY editChanged)
    Q_PROPERTY(double keptDuration READ keptDuration NOTIFY editChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY editChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY editChanged)
    // The finished take has not been saved as it is now edited.
    Q_PROPERTY(bool unsaved READ unsaved NOTIFY editChanged)
    Q_PROPERTY(QObject *thumbnails READ thumbnails CONSTANT)
public:
    explicit Backend(QObject *parent = nullptr);
    Backend(FilePicker *picker, bool activateHardware, QObject *parent = nullptr);
    ~Backend() override;
    QString state() const { return m_state; }
    QString message() const { return m_message; }
    QString formatLabel() const { return m_formatLabel; }
    QVariantList cameras() const { return m_cameras; }
    // Resolutions offered for the current camera, best first; the first is the camera's maximum.
    QVariantList cameraFormats() const { return m_cameraFormats; }
    int cameraFormatIndex() const { return m_cameraFormatIndex; }
    QVariantList microphones() const { return m_microphones; }
    int cameraIndex() const;
    int microphoneIndex() const;
    bool ready() const;
    bool audioEnabled() const { return m_audioId != "none"; }
    bool takeActive() const { return m_writer != nullptr; }
    bool dialogOpen() const { return m_dialogOpen; }
    double duration() const { return m_duration; }
    double level() const { return m_level; }
    double peakLevel() const { return m_peak; }
    bool clipping() const { return now() < m_clipUntil; }
    QString meterText() const;
    QUrl clip() const { return QUrl::fromLocalFile(m_clipPath); }
    double saveProgress() const { return m_saveProgress; }
    QVariantList clips() const;
    QVariantList pauses() const;
    double keptDuration() const { return edit::keptDuration(m_edit); }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    bool unsaved() const;
    Thumbnails *thumbnails() { return &m_thumbnails; }
    edit::Clips currentEdit() const { return m_edit; }
    Q_INVOKABLE void setPreview(QObject *sink);
    Q_INVOKABLE void selectCamera(int index);
    // Chooses one of the current camera's resolutions; index 0 is its maximum.
    Q_INVOKABLE void selectCameraFormat(int index);
    Q_INVOKABLE void selectMicrophone(int index);
    Q_INVOKABLE void retry();
    Q_INVOKABLE void toggleRecording();
    Q_INVOKABLE void finish();
    Q_INVOKABLE void newRecording();
    Q_INVOKABLE void save();
    Q_INVOKABLE void confirmOverwrite(bool confirmed);
    // Edits to the finished recording. A gesture (a drag) is one undo step.
    Q_INVOKABLE void split(double time);
    Q_INVOKABLE void setClip(int index, double start, double end);
    Q_INVOKABLE void removeClip(int index);
    // Joins clip index with the next one, restoring whatever lay between them.
    Q_INVOKABLE void joinClips(int index);
    Q_INVOKABLE void beginGesture();
    Q_INVOKABLE void endGesture();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    // Deletes the finished take on leaving the editor, e.g. when quitting.
    Q_INVOKABLE void closeTake();
    Q_INVOKABLE void discardAndClose();
    Q_INVOKABLE void discardCurrent();
signals:
    void changed();
    void devicesChanged();
    void meterChanged();
    void editChanged();
    void safeToClose();
    void overwriteRequested(const QString &path);
private:
    qint64 now() const { return m_wall.nsecsElapsed() / 1000; }
    void refreshDevices();
    void activateSources();
    // keepPicture leaves the last camera frame on screen, e.g. while a stopped take finalizes.
    void releaseSources(bool keepPicture = false);
    void readAudio();
    void receiveAudio(const QByteArray &data, qint64 capturedAt);
    void finishWhenCaptured();
    void receiveVideo(const QVideoFrame &frame);
    void tick();
    void sourceFailed(const QString &message);
    void startTake();
    void writerFinished();
    void probeClip(const QString &path, bool currentTake);
    void saveTo(const QUrl &url);
    void writeTo(const QString &destination);
    void copyTo(const QString &destination);
    void exportTo(const QString &destination);
    void saved(const QString &destination, const QString &error);
    void applyEdit(edit::Clips next);
    void resetEdit(const edit::Clips &clips, const QList<double> &pauses);
    // Takes are deleted when you leave them, so one left on disk was interrupted.
    void recoverTake();
    bool lockTake(const QString &directory);
    void deleteTake();
    void updateReady();
    QMediaDevices m_devices;
    QMediaCaptureSession m_capture;
    QVideoSink m_sink;
    QPointer<QVideoSink> m_preview;
    QCamera *m_camera = nullptr;
    AudioCapture *m_audio = nullptr;
    QAudioFormat m_audioFormat;
    QVideoFrame m_lastFrame;
    QCameraFormat m_cameraFormat;
    QElapsedTimer m_wall;
    QTimer m_tick;
    QTimer m_finishTimeout;
    QSettings m_settings;
    FilePicker *m_picker;
    bool m_activateHardware;
    QString m_pendingDestination;
    Writer *m_writer = nullptr;
    QVariantList m_cameras, m_microphones, m_cameraFormats;
    int m_cameraFormatIndex = 0;
    QString m_cameraId, m_audioId, m_state = "starting", m_message, m_formatLabel;
    QString m_root, m_takeId, m_clipPath, m_clipFileName;
    QString m_interruption;
    // Held while a take is in use, so another Monologue never mistakes it for an interrupted one.
    std::unique_ptr<QLockFile> m_takeLock;
    edit::Clips m_edit, m_savedEdit;
    QList<edit::Clips> m_undo, m_redo;
    QList<double> m_pauses;
    Thumbnails m_thumbnails;
    QProcess *m_export = nullptr;
    QString m_exportPartial;
    double m_saveProgress = 0;
    bool m_gesture = false, m_gestureRecorded = false, m_clipAudio = false;
    qint64 m_lastVideoAt = 0, m_lastAudioAt = 0, m_activatedAt = 0;
    qint64 m_videoOrigin = -1, m_videoBase = 0;
    qint64 m_audioCapturedUntil = -1, m_videoCapturedUntil = -1, m_finishAt = -1;
    qint64 m_clipUntil = 0, m_peakUntil = 0, m_signalAt = 0;
    double m_duration = 0, m_level = -60, m_peak = -60, m_pendingPeak = 0, m_fps = 30;
    bool m_cameraHealthy = false, m_audioHealthy = false, m_dialogOpen = false;
    bool m_hasSaved = false, m_discardAfter = false, m_restartAfter = false, m_probing = false;
};
