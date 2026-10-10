#include "backend.h"
#include "mediautils.h"
#include "windowcapture.h"
#include "windowlist.h"
#include <QAudioDevice>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDateTime>
#include <QTemporaryFile>
#include <QUuid>
#include <QProcess>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <cmath>
#include <cstdio>
#include <algorithm>

static QString deviceId(const QByteArray &id) { return QString::fromLatin1(id.toBase64()); }
static QString formatText(const QSize &size, double fps, bool upTo) {
    return QString("%1 × %2 · %3%4 fps").arg(size.width()).arg(size.height()).arg(upTo ? "up to " : "").arg(fps, 0, 'f', fps == int(fps) ? 0 : 2);
}
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
    m_windowList = new WindowList(this);
    connect(m_windowList, &WindowList::updated, this, &Backend::refreshDevices);
    if(m_activateHardware) {
        connect(&m_devices,&QMediaDevices::videoInputsChanged,this,&Backend::refreshDevices);
        connect(&m_devices,&QMediaDevices::audioInputsChanged,this,&Backend::refreshDevices);
    }
    connect(m_picker,&FilePicker::selected,this,&Backend::saveTo);
    connect(m_picker,&FilePicker::closed,this,[this] { m_dialogOpen=false; emit changed(); });
    connect(m_picker,&FilePicker::failed,this,[this](const QString &error) { m_dialogOpen=false; m_message=error; emit changed(); });
    m_tick.setInterval(40);
    connect(&m_tick,&QTimer::timeout,this,&Backend::tick);
    m_tick.start();
    m_finishTimeout.setSingleShot(true);
    m_finishTimeout.setInterval(2000);
    connect(&m_finishTimeout, &QTimer::timeout, this, [this] {
        if (!m_writer || m_finishAt < 0) return;
        if (m_interruption.isEmpty()) m_interruption = "The final capture buffers did not arrive, so the take ends a moment early.";
        m_finishAt = -1;
        m_writer->finish();
    });
    QTimer::singleShot(0,this,[this] {
        recoverTake();
        if(!m_activateHardware) return;
        m_windowList->start();
        refreshDevices();
    });
}
Backend::~Backend() {
    releaseSources();
    if(m_export) { m_export->disconnect(this); m_export->kill(); m_export->waitForFinished(); QFile::remove(m_exportPartial); }
}
int Backend::cameraIndex() const { return indexOf(m_cameras,m_cameraId); }
int Backend::microphoneIndex() const { return indexOf(m_microphones,m_audioId); }
bool Backend::ready() const { return m_state=="ready" && m_cameraHealthy && (!audioEnabled() || m_audioHealthy); }
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
    // Infrared sensors (for face login) appear as greyscale-only cameras; they can't record a usable take.
    QList<QCameraDevice> cameras;
    for(const auto &d:QMediaDevices::videoInputs()) {
        const auto formats=d.videoFormats();
        if(std::all_of(formats.begin(),formats.end(),[](const auto &f) {
            return f.pixelFormat()==QVideoFrameFormat::Format_Y8 || f.pixelFormat()==QVideoFrameFormat::Format_Y16;
        })) continue;
        cameras.append(d);
    }
    const auto microphones=QMediaDevices::audioInputs();
    if (m_cameraId.isEmpty() && !cameras.isEmpty()) {
        const auto preferred=QMediaDevices::defaultVideoInput();
        m_cameraId=deviceId((cameras.contains(preferred) ? preferred : cameras.first()).id());
    }
    if (m_audioId.isEmpty() && !microphones.isEmpty()) m_audioId=deviceId(QMediaDevices::defaultAudioInput().id());
    auto label=[](const auto &device,const auto &all) {
        int count=0;
        for(const auto &other:all) if(other.description()==device.description()) ++count;
        return count>1 ? device.description()+" · "+QString::fromUtf8(device.id()) : device.description();
    };
    QVariantList nextCameras, nextMicrophones;
    for(const auto &d:cameras) nextCameras.append(QVariantMap{{"id",deviceId(d.id())},{"label",label(d,cameras)},{"kind","camera"}});
    for(const auto &w:m_windowList->windows())
        nextCameras.append(QVariantMap{{"id",w.id},{"label",w.label},{"kind","window"},{"title",w.title},{"windowClass",w.windowClass},{"workspace",w.workspace},{"available",true}});
    for(const auto &d:microphones) nextMicrophones.append(QVariantMap{{"id",deviceId(d.id())},{"label",label(d,microphones)}});
    nextMicrophones.append(QVariantMap{{"id","none"},{"label","No audio"}});
    // The first client list has not arrived yet. Keep the remembered window
    // visible, and do not report it missing or fall back to a camera.
    const bool windowPending=window::isWindow(m_cameraId) && !m_windowList->settled();
    if(windowPending) nextCameras.prepend(QVariantMap{{"id",m_cameraId},{"label",m_settings.value("camera/label","Window").toString()},{"kind","window"},{"available",false}});
    const bool cameraMissing=indexOf(nextCameras,m_cameraId)<0;
    const bool audioMissing=indexOf(nextMicrophones,m_audioId)<0;
    if(cameraMissing) {
        const bool window=window::isWindow(m_cameraId);
        nextCameras.prepend(QVariantMap{{"id",m_cameraId},{"label",m_settings.value("camera/label",window?"Window":"Camera").toString()+" — unavailable"},{"kind",window?"window":"camera"},{"available",false}});
    }
    if(audioMissing) nextMicrophones.prepend(QVariantMap{{"id",m_audioId},{"label",m_settings.value("microphone/label","Microphone").toString()+" — unavailable"}});
    const bool same=nextCameras==m_cameras && nextMicrophones==m_microphones;
    m_cameras=nextCameras; m_microphones=nextMicrophones;
    if(!same) emit devicesChanged();
    if(windowPending) return;
    if(m_writer) {
        if(cameraMissing && window::isWindow(m_cameraId)) sourceFailed("The window closed, so the take was stopped.");
        else if(cameraMissing || audioMissing) sourceFailed("A selected source was disconnected, so the take was stopped.");
    } else if(m_state!="finished" && m_state!="saving" && m_state!="finalizing") {
        // Do not reopen healthy devices when an unrelated input is plugged in.
        if(!m_cameraHealthy || (audioEnabled() && !m_audioHealthy) || cameraMissing || audioMissing) activateSources();
    }
}
void Backend::releaseSources(bool keepPicture) {
    if(m_camera) { m_camera->disconnect(this); m_camera->stop(); m_capture.setCamera(nullptr); delete m_camera; m_camera=nullptr; }
    if(m_window) { m_window->disconnect(this); m_window->stop(); delete m_window; m_window=nullptr; }
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
    m_state="starting"; m_message.clear(); m_formatLabel.clear(); m_videoSize=QSize(); m_activatedAt=now();
    QCameraDevice selectedCamera;
    window::Info selectedWindow;
    bool haveWindow=false;
    if(window::isWindow(m_cameraId)) {
        for(const auto &candidate:m_windowList->windows()) if(candidate.id==m_cameraId) { selectedWindow=candidate; haveWindow=true; break; }
    } else for(const auto &d:QMediaDevices::videoInputs()) if(deviceId(d.id())==m_cameraId) selectedCamera=d;
    QAudioDevice selectedAudio;
    for(const auto &d:QMediaDevices::audioInputs()) if(deviceId(d.id())==m_audioId) selectedAudio=d;
    if(window::isWindow(m_cameraId)) {
        if(!haveWindow) { m_state="unavailable"; m_message="Choose an available video source."; }
        else {
            m_fps=30;
            m_formatLabel="Window · up to 30 fps";
            m_window=new WindowCapture(this);
            connect(m_window,&WindowCapture::frameReady,this,&Backend::receiveVideo);
            connect(m_window,&WindowCapture::failed,this,[this](const QString &error) {
                // The capture reports a closed window the same way during preview and a take.
                sourceFailed(!m_writer && error=="The window closed, so the take was stopped." ? "The window stopped delivering frames." : error);
            });
            m_window->start(selectedWindow.handle);
        }
    } else if(selectedCamera.isNull()) { m_state="unavailable"; m_message="Connect a camera or choose an available video source."; }
    else {
        m_cameraFormat=media::bestCameraFormat(selectedCamera);
        if(m_cameraFormat.isNull()) { m_state="unavailable"; m_message="This camera advertises no supported video formats."; }
        else {
            m_fps=media::targetFps(m_cameraFormat.minFrameRate(),m_cameraFormat.maxFrameRate());
            m_videoSize=m_cameraFormat.resolution();
            m_formatLabel=formatText(m_videoSize,m_fps,true);
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
    if((!m_camera && !m_window) || !frame.isValid() || !frame.size().isValid()) return;
    m_lastVideoAt=now();
    if(!m_videoSize.isValid()) {
        m_videoSize=frame.size();
        if(m_window) m_formatLabel=formatText(m_videoSize,m_fps,false);
    } else if(frame.size()!=m_videoSize) {
        if(m_window && !m_writer) {
            m_videoSize=frame.size();
            m_formatLabel=formatText(m_videoSize,m_fps,false);
            emit changed();
        } else if(m_window) { sourceFailed("The window changed size, so the take was stopped."); return; }
        else { sourceFailed("The camera did not provide its maximum resolution. Choose another source or Retry."); return; }
    }
    m_lastFrame=frame;
    // Once stopped, the preview holds the take's final frame while it finalizes.
    if(m_preview && m_state!="finalizing") m_preview->setVideoFrame(frame);
    if(!m_cameraHealthy) {
        m_cameraHealthy=true;
        m_settings.setValue("camera/id",m_cameraId);
        const auto entry=m_cameras.value(cameraIndex()).toMap().value("label").toString();
        m_settings.setValue("camera/label",m_camera ? m_camera->cameraDevice().description() : entry);
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
        if(m_window && time-m_lastVideoAt>3000000) sourceFailed("The window stopped delivering frames.");
        else if(m_camera && time-m_lastVideoAt>3000000) sourceFailed("The camera stopped delivering video. Check the connection, then Retry.");
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
void Backend::selectSource(const QString &id) { selectCamera(indexOf(m_cameras,id)); }
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
    if(!QDir().mkpath(directory) || !lockTake(directory)) { m_message="Could not create the recording folder. Check available space and permissions."; emit changed(); return; }
    m_clipFileName="Monologue-"+QDateTime::currentDateTime().toString("yyyy-MM-dd-HHmmss")+".mp4";
    m_clipPath=directory+"/"+m_clipFileName;
    m_interruption.clear(); m_duration=0;
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
    m_writer->deleteLater(); m_writer=nullptr;
    releaseSources(true);
    if(m_discardAfter) { m_discardAfter=false; deleteTake(); emit safeToClose(); return; }
    if(m_restartAfter) { m_restartAfter=false; deleteTake(); activateSources(); return; }
    probeClip(m_clipPath,true);
}
void Backend::probeClip(const QString &path,bool currentTake) {
    if(m_probing) return;
    m_probing=true; m_state="finalizing"; emit changed();
    auto *watcher=new QFutureWatcher<media::Probe>(this);
    connect(watcher,&QFutureWatcher<media::Probe>::finished,this,[this,watcher,path,currentTake] {
        const auto result=watcher->result(); watcher->deleteLater(); m_probing=false;
        if(m_discardAfter) { m_discardAfter=false; deleteTake(); emit safeToClose(); return; }
        const bool correct=currentTake ? result.size==m_videoSize && result.audio==audioEnabled() : true;
        if(result.ok && correct) {
            m_clipPath=path; m_clipFileName=QFileInfo(path).fileName(); m_duration=result.duration;
            m_state="finished"; m_message=m_interruption; m_clipAudio=result.audio;
            resetEdit(edit::whole(result.duration),m_pauses);
            m_thumbnails.load(path,result.duration);
        } else {
            // Nothing here can be edited; don't leave it to be "recovered" again next launch.
            deleteTake();
            m_state="unavailable";
            m_message=!currentTake ? "Your last take was interrupted and could not be recovered."
                    : result.ok ? "The encoded clip did not match the selected resolution or audio mode." : result.error;
        }
        emit changed();
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
    m_picker->saveVideo(QUrl::fromLocalFile(directory+"/"+m_clipFileName));
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
    // Takes are deleted when you leave them, so a save must never land among them.
    const auto folder=QFileInfo(QFileInfo(destination).absolutePath()).canonicalFilePath();
    const auto takes=QFileInfo(m_root).canonicalFilePath();
    if(!takes.isEmpty() && (folder==takes || folder.startsWith(takes+"/"))) {
        m_message="Could not save: choose a folder outside Monologue's working files."; emit changed(); return;
    }
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
    // Encode into a freshly reserved file beside the destination, then rename it into place.
    const QFileInfo target(destination);
    QTemporaryFile partial(target.absolutePath()+"/."+target.completeBaseName()+"-XXXXXX.mp4");
    partial.setAutoRemove(false);
    if(!partial.open()) { m_message="Could not save: "+partial.errorString(); emit changed(); return; }
    m_exportPartial=partial.fileName();
    partial.close();
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
    deleteTake();
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
    // Normalizing can drop a clip too short to keep; never let that empty the take.
    const auto next=edit::normalized(m_edit,m_duration);
    if(!next.isEmpty() && next!=m_edit) { m_edit=next; emit editChanged(); }
}
void Backend::undo() {
    if(m_state!="finished" || m_gesture || m_undo.isEmpty()) return;
    m_redo.append(m_edit); m_edit=m_undo.takeLast(); emit editChanged();
}
void Backend::redo() {
    if(m_state!="finished" || m_gesture || m_redo.isEmpty()) return;
    m_undo.append(m_edit); m_edit=m_redo.takeLast(); emit editChanged();
}
bool Backend::lockTake(const QString &directory) {
    auto lock=std::make_unique<QLockFile>(directory+"/.lock");
    // Only a dead owner makes a lock stale; a take can be recorded and edited for hours.
    lock->setStaleLockTime(0);
    if(!lock->tryLock(0)) return false;
    m_takeLock=std::move(lock);
    return true;
}
void Backend::deleteTake() {
    if(!m_takeId.isEmpty()) {
        m_takeLock.reset();
        QDir(m_root+"/"+m_takeId).removeRecursively();
    }
    m_takeId.clear(); m_clipPath.clear(); m_clipFileName.clear();
    resetEdit({},{}); m_thumbnails.clear();
}
void Backend::recoverTake() {
    const auto entries=QDir(m_root).entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Time);
    for(const auto &entry:entries) {
        if(!lockTake(entry.filePath())) continue; // Another Monologue is using it.
        QString clip;
        for(const auto &name:QDir(entry.filePath()).entryList({"*.mp4"},QDir::Files))
            if(!name.endsWith(".finalizing.mp4")) clip=name;
        m_takeId=entry.fileName();
        // Only a recent take is worth reopening; anything older is just left over.
        const QFileInfo file(entry.filePath()+"/"+clip);
        if(clip.isEmpty() || file.lastModified().secsTo(QDateTime::currentDateTime())>=3600) { deleteTake(); continue; }
        m_interruption="Recovered the take from your last session.";
        m_pauses.clear();
        probeClip(entry.filePath()+"/"+clip,false);
        return;
    }
}
void Backend::discardAndClose() {
    // The take may already have stopped on its own while the quit dialog was open.
    if(m_writer) { m_discardAfter=true; finish(); return; }
    if(m_probing) { m_discardAfter=true; return; }
    closeTake(); emit safeToClose();
}
void Backend::discardCurrent() {
    if(m_dialogOpen || m_probing || m_state=="saving" || m_state=="finalizing") return;
    if(m_writer) { m_restartAfter=true; finish(); }
    else if(m_state=="finished") newRecording();
}
