#include "backend.h"
#include "mediautils.h"
#include <QAudioDevice>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSaveFile>
#include <QUuid>
#include <QProcess>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QRegularExpression>
#include <cmath>
#include <cstdio>
#include <algorithm>

static QString deviceId(const QByteArray &id) { return QString::fromLatin1(id.toBase64()); }
static int indexOf(const QVariantList &list, const QString &id) {
    for (int i=0; i<list.size(); ++i) if (list[i].toMap()["id"].toString()==id) return i;
    return -1;
}
Backend::Backend(QObject *parent) : Backend(new PortalFilePicker, true, parent) {}
Backend::Backend(FilePicker *picker, bool activateHardware, QObject *parent)
    : QObject(parent), m_picker(picker), m_activateHardware(activateHardware) {
    if(!m_picker->parent()) m_picker->setParent(this);
    m_wall.start();
    m_root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/recordings";
    m_cameraId=m_settings.value("camera/id").toString();
    m_audioId=m_settings.value("microphone/id").toString();
    m_capture.setVideoSink(&m_sink);
    connect(&m_sink,&QVideoSink::videoFrameChanged,this,&Backend::receiveVideo);
    if(m_activateHardware) {
        connect(&m_devices,&QMediaDevices::videoInputsChanged,this,&Backend::refreshDevices);
        connect(&m_devices,&QMediaDevices::audioInputsChanged,this,&Backend::refreshDevices);
    }
    connect(m_picker,&FilePicker::exportSelected,this,[this](const QUrl &url,double,double,int) { saveTo(url); });
    connect(m_picker,&FilePicker::closed,this,[this] { m_dialogOpen=false; emit changed(); });
    connect(m_picker,&FilePicker::failed,this,[this](const QString &error) { m_dialogOpen=false; m_message=error; emit changed(); });
    m_tick.setInterval(40);
    connect(&m_tick,&QTimer::timeout,this,&Backend::tick);
    m_tick.start();
    m_finishTimeout.setSingleShot(true);
    m_finishTimeout.setInterval(2000);
    connect(&m_finishTimeout, &QTimer::timeout, this, [this] {
        if (!m_writer || m_finishAt < 0) return;
        if (m_interruption.isEmpty()) m_interruption = "The final capture buffers did not arrive. The interrupted take has been kept.";
        m_finishAt = -1;
        m_writer->finish();
    });
    QTimer::singleShot(0,this,[this] { if(m_activateHardware) refreshDevices(); refreshRecordings(); });
}
Backend::~Backend() {
    releaseSources();
    if(m_export) { m_export->disconnect(this); m_export->kill(); m_export->waitForFinished(); QFile::remove(m_exportPartial); }
}
int Backend::cameraIndex() const { return indexOf(m_cameras,m_cameraId); }
int Backend::microphoneIndex() const { return indexOf(m_microphones,m_audioId); }
bool Backend::ready() const { return m_state=="ready" && m_cameraHealthy && (!audioEnabled() || m_audioHealthy); }
QString Backend::clipName() const { return m_clipFileName; }
QString Backend::meterText() const {
    if (!audioEnabled()) return "Audio off";
    if (!m_audioHealthy) return "Unavailable";
    if (clipping()) return "Clipping";
    if (now()-m_signalAt > 3000000) return "No signal";
    return QString::number(qRound(m_level)) + " dBFS";
}
void Backend::setPreview(QObject *sink) {
    m_preview=qobject_cast<QVideoSink*>(sink);
    if (m_preview && m_lastFrame.isValid()) m_preview->setVideoFrame(m_lastFrame);
}
void Backend::refreshDevices() {
    const auto cameras=QMediaDevices::videoInputs();
    const auto microphones=QMediaDevices::audioInputs();
    if (m_cameraId.isEmpty() && !cameras.isEmpty()) m_cameraId=deviceId(QMediaDevices::defaultVideoInput().id());
    if (m_audioId.isEmpty() && !microphones.isEmpty()) m_audioId=deviceId(QMediaDevices::defaultAudioInput().id());
    auto label=[](const auto &device,const auto &all) {
        int count=0;
        for(const auto &other:all) if(other.description()==device.description()) ++count;
        return count>1 ? device.description()+" · "+QString::fromUtf8(device.id()) : device.description();
    };
    m_cameras.clear(); m_microphones.clear();
    for(const auto &d:cameras) m_cameras.append(QVariantMap{{"id",deviceId(d.id())},{"label",label(d,cameras)}});
    for(const auto &d:microphones) m_microphones.append(QVariantMap{{"id",deviceId(d.id())},{"label",label(d,microphones)}});
    m_microphones.append(QVariantMap{{"id","none"},{"label","No audio"}});
    const bool cameraMissing=indexOf(m_cameras,m_cameraId)<0;
    const bool audioMissing=indexOf(m_microphones,m_audioId)<0;
    if(cameraMissing) m_cameras.prepend(QVariantMap{{"id",m_cameraId},{"label",m_settings.value("camera/label","Camera").toString()+" — unavailable"}});
    if(audioMissing) m_microphones.prepend(QVariantMap{{"id",m_audioId},{"label",m_settings.value("microphone/label","Microphone").toString()+" — unavailable"}});
    emit devicesChanged();
    if(m_writer) {
        if(cameraMissing || audioMissing) sourceFailed("A selected source was disconnected. The interrupted take has been kept.");
    } else if(m_state!="finished" && m_state!="saving" && m_state!="finalizing") {
        // Do not reopen healthy devices when an unrelated input is plugged in.
        if(!m_cameraHealthy || (audioEnabled() && !m_audioHealthy) || cameraMissing || audioMissing) activateSources();
    }
}
void Backend::releaseSources(bool keepPicture) {
    if(m_camera) { m_camera->disconnect(this); m_camera->stop(); m_capture.setCamera(nullptr); delete m_camera; m_camera=nullptr; }
    if(m_audio) { m_audio->disconnect(this); m_audio->stop(); delete m_audio; m_audio=nullptr; }
    m_cameraHealthy=false; m_audioHealthy=false;
    m_videoOrigin=-1; m_audioCapturedUntil=-1; m_videoCapturedUntil=-1;
    m_lastFrame=QVideoFrame();
    if(m_preview && !keepPicture) m_preview->setVideoFrame({});
    m_level=m_peak=-60; emit meterChanged();
}
void Backend::activateSources() {
    if(m_writer || m_state=="saving" || m_probing) return;
    if(!m_activateHardware) { m_state="unavailable"; emit changed(); return; }
    releaseSources();
    m_state="starting"; m_message.clear(); m_formatLabel.clear(); m_activatedAt=now();
    QCameraDevice selectedCamera;
    for(const auto &d:QMediaDevices::videoInputs()) if(deviceId(d.id())==m_cameraId) selectedCamera=d;
    QAudioDevice selectedAudio;
    for(const auto &d:QMediaDevices::audioInputs()) if(deviceId(d.id())==m_audioId) selectedAudio=d;
    if(selectedCamera.isNull()) { m_state="unavailable"; m_message="Connect a camera or choose an available video source."; }
    else {
        m_cameraFormat=media::bestCameraFormat(selectedCamera);
        if(m_cameraFormat.isNull()) { m_state="unavailable"; m_message="This camera advertises no supported video formats."; }
        else {
            m_fps=media::targetFps(m_cameraFormat.minFrameRate(),m_cameraFormat.maxFrameRate());
            m_formatLabel=QString("%1 × %2 · up to %3 fps").arg(m_cameraFormat.resolution().width()).arg(m_cameraFormat.resolution().height()).arg(m_fps,0,'f',m_fps==int(m_fps)?0:2);
            m_camera=new QCamera(selectedCamera,this);
            m_camera->setCameraFormat(m_cameraFormat);
            m_capture.setCamera(m_camera);
            connect(m_camera,&QCamera::errorOccurred,this,[this](auto,const QString &error) { sourceFailed("Camera: "+error); });
            m_camera->start();
        }
    }
    m_audioFormat=QAudioFormat();
    if(audioEnabled()) {
        if(selectedAudio.isNull()) { m_state="unavailable"; m_message="Choose an available microphone, or select No audio."; }
        else {
            m_audioFormat=selectedAudio.preferredFormat();
            m_audio=new AudioCapture([this] { return now(); },this);
            connect(m_audio,&AudioCapture::samples,this,&Backend::receiveAudio);
            connect(m_audio,&AudioCapture::failed,this,&Backend::sourceFailed);
            m_audio->start(selectedAudio.id(),m_audioFormat);
        }
    } else { m_settings.setValue("microphone/id","none"); m_settings.setValue("microphone/label","No audio"); }
    if(QStandardPaths::findExecutable("ffprobe").isEmpty() || QStandardPaths::findExecutable("ffmpeg").isEmpty()) { m_state="unavailable"; m_message="Install ffmpeg (including ffprobe) to record and inspect clips."; }
    else if(!Writer::supported(audioEnabled())) { m_state="unavailable"; m_message="This Qt multimedia backend cannot encode H.264 MP4 with the selected audio mode."; }
    emit changed();
}
void Backend::updateReady() {
    if(m_state=="starting" && m_cameraHealthy && (!audioEnabled() || m_audioHealthy)) {
        m_state="ready"; m_message.clear(); emit changed();
    }
}
void Backend::receiveVideo(const QVideoFrame &frame) {
    if(!m_camera || !frame.isValid()) return;
    m_lastVideoAt=now();
    if(frame.size()!=m_cameraFormat.resolution()) {
        sourceFailed("The camera did not provide its maximum resolution. Choose another source or Retry."); return;
    }
    m_lastFrame=frame;
    // Once stopped, the preview holds the take's final frame while it finalizes.
    if(m_preview && m_state!="finalizing") m_preview->setVideoFrame(frame);
    if(!m_cameraHealthy) {
        m_cameraHealthy=true;
        m_settings.setValue("camera/id",m_cameraId);
        m_settings.setValue("camera/label",m_camera->cameraDevice().description());
        updateReady();
    }
    qint64 captureTime=m_lastVideoAt;
    if(frame.startTime()>=0) {
        if(m_videoOrigin<0 || frame.startTime()<m_videoOrigin) { m_videoOrigin=frame.startTime(); m_videoBase=m_lastVideoAt; }
        captureTime=m_videoBase+frame.startTime()-m_videoOrigin;
        // Re-anchor after a device timestamp discontinuity rather than admitting
        // future frames into the take's compact timeline.
        if(captureTime>m_lastVideoAt || m_lastVideoAt-captureTime>500000) { m_videoOrigin=frame.startTime(); m_videoBase=m_lastVideoAt; captureTime=m_lastVideoAt; }
    }
    m_videoCapturedUntil = std::max(m_videoCapturedUntil, captureTime);
    if(m_writer) m_writer->video(frame,captureTime);
    finishWhenCaptured();
}
void Backend::readAudio() {
    if(m_audio) m_audio->readAvailable();
}
void Backend::receiveAudio(const QByteArray &data, qint64 capturedAt) {
    if(data.isEmpty() || !m_audioFormat.isValid()) return;
    m_lastAudioAt=now();
    if(!m_audioHealthy) {
        m_audioHealthy=true;
        m_settings.setValue("microphone/id",m_audioId);
        if(m_audio) m_settings.setValue("microphone/label",m_microphones.value(microphoneIndex()).toMap()["label"]);
        updateReady();
    }
    m_pendingPeak=std::max(m_pendingPeak,media::peak(data,m_audioFormat));
    m_audioCapturedUntil = std::max(m_audioCapturedUntil, capturedAt + m_audioFormat.durationForBytes(data.size()));
    if(m_writer) m_writer->audio(data,capturedAt);
    finishWhenCaptured();
}
void Backend::tick() {
    const auto time=now();
    const double db=m_pendingPeak>0 ? std::max(-60.0,20*std::log10(m_pendingPeak)) : -60;
    m_level=std::max(db,m_level-1.5);
    if(db>-60) m_signalAt=time;
    if(m_pendingPeak>=.999) m_clipUntil=time+1000000;
    if(db>=m_peak) { m_peak=db; m_peakUntil=time+700000; }
    else if(time>m_peakUntil) m_peak=std::max(m_level,m_peak-1.5);
    m_pendingPeak=0; emit meterChanged();
    if(m_writer && (m_state=="recording" || m_state=="paused")) { m_duration=m_writer->duration(time)/1000000.0; emit changed(); }
    if((m_state=="starting" || m_state=="ready" || m_state=="recording" || m_state=="paused") && time-m_activatedAt>5000000) {
        if(m_camera && time-m_lastVideoAt>3000000) sourceFailed("The camera stopped delivering video. Check the connection, then Retry.");
        else if(audioEnabled() && m_audio && time-m_lastAudioAt>3000000) sourceFailed("The microphone stopped delivering audio. Check the connection, then Retry.");
    }
}
void Backend::sourceFailed(const QString &message) {
    if(m_state=="finished" || m_state=="saving" || m_state=="finalizing") return;
    m_message=message;
    if(m_writer) { m_interruption=message; finish(); }
    else m_state="unavailable";
    emit changed();
}
void Backend::selectCamera(int index) {
    if(m_writer || index<0 || index>=m_cameras.size() || m_state=="saving" || m_probing) return;
    m_cameraId=m_cameras[index].toMap()["id"].toString(); activateSources();
}
void Backend::selectMicrophone(int index) {
    if(m_writer || index<0 || index>=m_microphones.size() || m_state=="saving" || m_probing) return;
    m_audioId=m_microphones[index].toMap()["id"].toString(); activateSources();
}
void Backend::retry() { if(!m_writer && m_state!="finished") activateSources(); }
void Backend::toggleRecording() {
    if(m_dialogOpen) return;
    if(ready()) { startTake(); return; }
    if(!m_writer) return;
    if(m_state=="recording") { readAudio(); m_writer->pause(now()); m_state="paused"; }
    else if(m_state=="paused") {
        // Drain the device while still paused, so queued rehearsal audio is omitted.
        readAudio(); m_writer->resume(now()); m_state="recording";
    }
    emit changed();
}
void Backend::startTake() {
    if(!ready() || !m_lastFrame.isValid()) return;
    m_takeId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString directory=m_root+"/"+m_takeId;
    if(!QDir().mkpath(directory)) { m_message="Could not create the recording folder. Check available space and permissions."; emit changed(); return; }
    m_clipFileName="Monologue-"+QDateTime::currentDateTime().toString("yyyy-MM-dd-HHmmss")+".mp4";
    m_clipPath=directory+"/"+m_clipFileName;
    m_interruption.clear(); m_duration=0;
    writeManifest("in-progress");
    readAudio();
    m_writer=new Writer(this);
    connect(m_writer,&Writer::finished,this,&Backend::writerFinished,Qt::QueuedConnection);
    connect(m_writer,&Writer::failed,this,[this](const QString &error) { m_interruption=error; m_message=error; m_state="finalizing"; emit changed(); });
    m_state="recording"; m_message.clear();
    const auto start=now();
    if(m_writer->start(m_clipPath,m_lastFrame.surfaceFormat(),m_fps,m_audioFormat,start))
        m_writer->video(m_lastFrame,start);
    emit changed();
}
void Backend::finish() {
    if(!m_writer || m_state=="finalizing") return;
    if(m_state=="recording") readAudio();
    m_finishAt=now();
    m_duration=m_writer->duration(m_finishAt)/1000000.0;
    m_writer->pause(m_finishAt);
    m_state="finalizing"; m_message=m_interruption.isEmpty()?"Finishing your clip…":m_interruption;
    m_finishTimeout.start();
    finishWhenCaptured();
    emit changed();
}
void Backend::finishWhenCaptured() {
    if(!m_writer || m_finishAt < 0) return;
    if(m_videoCapturedUntil < m_finishAt || (audioEnabled() && m_audioCapturedUntil < m_finishAt)) return;
    m_finishAt=-1;
    m_finishTimeout.stop();
    m_writer->finish();
}
void Backend::writerFinished() {
    if(!m_writer) return;
    m_finishTimeout.stop(); m_finishAt=-1;
    m_pauses.clear();
    for(const auto join:m_writer->joins()) m_pauses.append(join/1000000.0);
    m_storedEdit={};
    m_writer->deleteLater(); m_writer=nullptr;
    releaseSources(true);
    if(m_discardAfter) {
        QDir(m_root+"/"+m_takeId).removeRecursively(); emit safeToClose(); return;
    }
    if(m_restartAfter) {
        m_restartAfter=false;
        if(QDir(m_root+"/"+m_takeId).removeRecursively()) {
            newRecording(); refreshRecordings(); return;
        }
        m_interruption="Could not discard the recording. The clip has been kept.";
    }
    probeClip(m_clipPath,true);
}
void Backend::probeClip(const QString &path,bool currentTake) {
    if(m_probing) return;
    m_probing=true; m_state="finalizing"; emit changed();
    auto *watcher=new QFutureWatcher<media::Probe>(this);
    connect(watcher,&QFutureWatcher<media::Probe>::finished,this,[this,watcher,path,currentTake] {
        const auto result=watcher->result(); watcher->deleteLater(); m_probing=false;
        const bool correct=currentTake ? result.size==m_cameraFormat.resolution() && result.audio==audioEnabled() : true;
        if(result.ok && correct) {
            m_clipPath=path; m_clipFileName=QFileInfo(path).fileName(); m_duration=result.duration;
            m_formatLabel=QString("%1 × %2 · %3").arg(result.size.width()).arg(result.size.height()).arg(result.audio?"Audio":"No audio");
            m_state="finished"; m_message=m_interruption; m_clipAudio=result.audio;
            resetEdit(edit::fromJson(m_storedEdit,result.duration),m_pauses);
            writeManifest(m_interruption.isEmpty()?"complete":"interrupted");
            m_thumbnails.load(path,result.duration);
        } else {
            m_state="unavailable";
            m_message=result.ok ? "The encoded clip did not match the selected resolution or audio mode. Its files have been kept in Recordings." : result.error;
            if(currentTake) writeManifest("incomplete");
            m_clipPath.clear();
        }
        refreshRecordings(); emit changed();
    });
    const auto fps=m_fps;
    watcher->setFuture(QtConcurrent::run([path,currentTake,fps] {
        if(currentTake) {
            const auto error=media::normalizeMp4(path,fps);
            if(!error.isEmpty()) { media::Probe failure; failure.error=error; return failure; }
        }
        return media::probe(path);
    }));
}
void Backend::newRecording() {
    if(m_writer || m_probing || m_state=="saving" || m_dialogOpen) return;
    closeTake();
    m_clipPath.clear(); m_clipFileName.clear(); m_takeId.clear(); m_duration=0;
    resetEdit({},{}); m_thumbnails.clear();
    activateSources();
}
void Backend::save() {
    if(m_state!="finished" || m_dialogOpen) return;
    const QString directory=m_settings.value("saveDirectory",QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString();
    m_dialogOpen=true; emit changed();
    m_picker->exportVideo(QUrl::fromLocalFile(directory+"/"+m_clipFileName),0,m_duration,{});
}
void Backend::saveTo(const QUrl &url) {
    if(m_state!="finished") return;
    if(!url.isLocalFile()) { m_message="Choose a local folder for the MP4 file."; emit changed(); return; }
    auto destination=url.toLocalFile();
    if(!destination.endsWith(".mp4",Qt::CaseInsensitive)) {
        destination+=".mp4";
        // The portal confirmed the selected path, not a different path formed
        // by appending the suffix. Ask before replacing that existing file.
        if(QFileInfo::exists(destination)) {
            m_pendingDestination=destination; m_dialogOpen=true;
            emit changed(); emit overwriteRequested(destination); return;
        }
    }
    writeTo(destination);
}
void Backend::confirmOverwrite(bool confirmed) {
    if(m_pendingDestination.isEmpty()) return;
    const auto destination=std::exchange(m_pendingDestination,{});
    m_dialogOpen=false; emit changed();
    if(confirmed) writeTo(destination);
}
void Backend::writeTo(const QString &destination) {
    if(m_state!="finished") return;
    if(edit::untouched(m_edit,m_duration)) copyTo(destination);
    else exportTo(destination);
}
void Backend::copyTo(const QString &destination) {
    if(m_state!="finished") return;
    const auto source=m_clipPath;
    m_state="saving"; m_message="Saving your clip…"; emit changed();
    auto *watcher=new QFutureWatcher<QString>(this);
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,watcher,destination] {
        const auto error=watcher->result(); watcher->deleteLater();
        saved(destination,error);
    });
    watcher->setFuture(QtConcurrent::run([source,destination] { return media::copyAtomically(source,destination); }));
}
void Backend::exportTo(const QString &destination) {
    if(m_state!="finished" || m_export) return;
    const QFileInfo target(destination);
    if(target.exists() && target.canonicalFilePath()==QFileInfo(m_clipPath).canonicalFilePath()) {
        m_message="Could not save: Choose a destination outside the retained recording itself."; emit changed(); return;
    }
    // Encode beside the destination, then rename over it once complete.
    m_exportPartial=target.absolutePath()+"/."+target.completeBaseName()+".monologue-partial.mp4";
    const double total=keptDuration();
    m_state="saving"; m_message="Saving your clip…"; m_saveProgress=0; emit changed();
    m_export=new QProcess(this);
    connect(m_export,&QProcess::readyReadStandardOutput,this,[this,total] {
        while(m_export->canReadLine()) {
            const auto line=m_export->readLine().trimmed();
            if(line.startsWith("out_time_us=") && total>0) {
                m_saveProgress=std::clamp(line.mid(12).toDouble()/1000000.0/total,0.0,1.0); emit changed();
            }
        }
    });
    connect(m_export,&QProcess::finished,this,[this,destination](int code,QProcess::ExitStatus status) {
        auto *process=std::exchange(m_export,nullptr);
        process->deleteLater();
        QString error;
        if(status!=QProcess::NormalExit || code!=0) {
            error=QString::fromUtf8(process->readAllStandardError()).trimmed().left(500);
            if(error.isEmpty()) error="ffmpeg could not encode the clip.";
        } else if(std::rename(QFile::encodeName(m_exportPartial).constData(),QFile::encodeName(destination).constData())!=0)
            error="Could not move the finished MP4 into place.";
        saved(destination,error);
    });
    connect(m_export,&QProcess::errorOccurred,this,[this,destination](QProcess::ProcessError error) {
        if(error!=QProcess::FailedToStart) return;
        m_export->deleteLater(); m_export=nullptr;
        saved(destination,"ffmpeg could not be started.");
    });
    m_export->start("ffmpeg",media::exportArgs(m_clipPath,m_exportPartial,edit::kept(m_edit),m_clipAudio));
}
void Backend::saved(const QString &destination,const QString &error) {
    if(!error.isEmpty() && !m_exportPartial.isEmpty()) QFile::remove(m_exportPartial);
    m_exportPartial.clear();
    m_state="finished"; m_saveProgress=0;
    m_message=error.isEmpty()?"Saved to "+destination:"Could not save: "+error;
    if(error.isEmpty()) {
        m_settings.setValue("saveDirectory",QFileInfo(destination).absolutePath());
        m_savedEdit=m_edit; m_hasSaved=true;
        emit editChanged();
    }
    emit changed();
}
bool Backend::unsaved() const {
    return (m_state=="finished" || m_state=="saving") && (!m_hasSaved || m_savedEdit!=m_edit);
}
void Backend::closeTake() {
    // Leaving the editor: the take has served its purpose, whether or not it was saved.
    if(m_writer || m_probing || m_state=="saving" || m_takeId.isEmpty()) return;
    if(auto directory=recordingDirectory(m_takeId); !directory.isEmpty()) QDir(directory).removeRecursively();
    m_takeId.clear(); m_clipPath.clear(); m_clipFileName.clear();
    if(m_state=="finished") m_state="closed";
    resetEdit({},{}); m_thumbnails.clear();
    emit changed();
}
QVariantList Backend::clips() const {
    QVariantList result;
    for(const auto &clip:m_edit) result.append(QVariantMap{{"start",clip.start},{"end",clip.end}});
    return result;
}
QVariantList Backend::pauses() const {
    QVariantList result;
    for(const auto pause:m_pauses) if(pause>0 && pause<m_duration) result.append(pause);
    return result;
}
void Backend::resetEdit(const edit::Clips &clips,const QList<double> &pauses) {
    m_edit=clips; m_pauses=pauses; m_undo.clear(); m_redo.clear(); m_gesture=false;
    m_savedEdit.clear(); m_hasSaved=false;
    emit editChanged();
}
void Backend::applyEdit(edit::Clips next) {
    if(m_state!="finished") return;
    // Mid-drag, clips keep their indices; the gesture's end normalizes.
    if(!m_gesture) next=edit::normalized(next,m_duration);
    if(next==m_edit || next.isEmpty()) return;
    if(!m_gesture || !m_gestureRecorded) { m_undo.append(m_edit); m_redo.clear(); m_gestureRecorded=m_gesture; }
    m_edit=next;
    if(!m_gesture) writeManifest(m_status);
    emit editChanged();
}
void Backend::split(double time) {
    for(int i=0;i<m_edit.size();++i) {
        const auto clip=m_edit[i];
        if(time-clip.start<edit::minimumClip || clip.end-time<edit::minimumClip) continue;
        auto next=m_edit;
        next[i].end=time; next.insert(i+1,{time,clip.end});
        applyEdit(next); return;
    }
}
void Backend::setClip(int index,double start,double end) {
    if(index<0 || index>=m_edit.size()) return;
    // A clip can grow into a gap, never over its neighbours.
    const double low=index>0 ? m_edit[index-1].end : 0;
    const double high=index+1<m_edit.size() ? m_edit[index+1].start : m_duration;
    start=std::clamp(start,low,high); end=std::clamp(end,low,high);
    if(end-start<edit::minimumClip) return;
    auto next=m_edit; next[index]={start,end};
    applyEdit(next);
}
void Backend::removeClip(int index) {
    if(index<0 || index>=m_edit.size() || m_edit.size()<2) return;
    auto next=m_edit; next.removeAt(index); applyEdit(next);
}
void Backend::joinClips(int index) {
    if(index<0 || index+1>=m_edit.size()) return;
    auto next=m_edit; next[index].end=next[index+1].end; next.removeAt(index+1); applyEdit(next);
}
void Backend::beginGesture() { if(m_state=="finished") { m_gesture=true; m_gestureRecorded=false; } }
void Backend::endGesture() {
    if(!m_gesture) return;
    m_gesture=false;
    const auto next=edit::normalized(m_edit,m_duration);
    if(next!=m_edit) { m_edit=next; emit editChanged(); }
    writeManifest(m_status);
}
void Backend::undo() {
    if(m_state!="finished" || m_gesture || m_undo.isEmpty()) return;
    m_redo.append(m_edit); m_edit=m_undo.takeLast(); writeManifest(m_status); emit editChanged();
}
void Backend::redo() {
    if(m_state!="finished" || m_gesture || m_redo.isEmpty()) return;
    m_undo.append(m_edit); m_edit=m_redo.takeLast(); writeManifest(m_status); emit editChanged();
}
void Backend::writeManifest(const QString &status) {
    m_status=status;
    QSaveFile file(m_root+"/"+m_takeId+"/take.json");
    if(!file.open(QIODevice::WriteOnly)) return;
    QJsonArray pauses;
    for(const auto pause:m_pauses) pauses.append(pause);
    QJsonObject manifest{{"filename",m_clipFileName},{"status",status},{"duration",m_duration},{"message",m_interruption},{"pauses",pauses}};
    if(m_state=="finished" || m_state=="saving") manifest["edit"]=edit::toJson(m_edit);
    file.write(QJsonDocument(manifest).toJson());
    file.commit();
}
QString Backend::recordingDirectory(const QString &id) const {
    static const QRegularExpression uuid("^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$");
    if(!uuid.match(id).hasMatch()) return {};
    const QFileInfo directory(m_root+"/"+id);
    if(directory.isSymLink() || !directory.isDir()) return {};
    return directory.absoluteFilePath();
}
void Backend::refreshRecordings() {
    m_recordings.clear();
    const auto entries=QDir(m_root).entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Time);
    for(const auto &entry:entries) {
        if(recordingDirectory(entry.fileName()).isEmpty()) continue;
        QFile manifest(entry.filePath()+"/take.json");
        if(!manifest.open(QIODevice::ReadOnly)) continue;
        const auto info=QJsonDocument::fromJson(manifest.readAll()).object();
        const QString name=info["filename"].toString();
        if(name.isEmpty() || QFileInfo(name).fileName()!=name) continue;
        const QFileInfo clip(entry.filePath()+"/"+name);
        m_recordings.append(QVariantMap{{"id",entry.fileName()},{"name",name},{"status",info["status"].toString()},{"size",QString::number(clip.size()/1048576.0,'f',1)+" MB"},{"path",clip.absoluteFilePath()}});
    }
    emit recordingsChanged();
}
void Backend::openRecording(const QString &id) {
    if(m_writer || m_probing || m_state=="saving" || m_dialogOpen) return;
    for(const auto &entry:m_recordings) {
        const auto item=entry.toMap();
        if(item["id"]==id && !recordingDirectory(id).isEmpty()) {
            releaseSources(); m_takeId=id; m_interruption.clear(); m_storedEdit={}; m_pauses.clear();
            QFile manifest(recordingDirectory(id)+"/take.json");
            if(manifest.open(QIODevice::ReadOnly)) {
                const auto info=QJsonDocument::fromJson(manifest.readAll()).object();
                m_interruption=info["message"].toString();
                m_storedEdit=info["edit"].toObject();
                m_pauses.clear();
                for(const auto &pause:info["pauses"].toArray()) m_pauses.append(pause.toDouble());
                if(info["status"]!="complete" && m_interruption.isEmpty()) m_interruption="Recovered after an interrupted recording.";
            }
            probeClip(item["path"].toString(),false); return;
        }
    }
}
void Backend::discardRecording(const QString &id) {
    if(m_writer || m_probing || m_state=="saving" || m_dialogOpen) return;
    const auto directory=recordingDirectory(id);
    if(directory.isEmpty()) return;
    if(!QDir(directory).removeRecursively()) { m_message="Could not discard the recording."; emit changed(); return; }
    if(id==m_takeId) newRecording();
    refreshRecordings();
}
void Backend::discardAndClose() { m_discardAfter=true; finish(); }
void Backend::discardCurrent() {
    if(m_dialogOpen || m_probing || m_state=="saving" || m_state=="finalizing") return;
    if(m_writer) { m_restartAfter=true; finish(); }
    else if(m_state=="finished") discardRecording(m_takeId);
}
