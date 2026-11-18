// Explicit, opt-in real-device check. Never part of bin/test.
#include <QGuiApplication>
#include <QTimer>
#include <QDebug>
#include <QTextStream>
#include <QDir>
#include <QStandardPaths>
#include <QProcess>
#include <QVideoSink>
#include "backend.h"
#include "mediautils.h"
// Video frames in the clip, counted by ffprobe; -1 when it cannot say.
static int clipFrames(const QString &path) {
    QProcess probe;
    probe.start("ffprobe",{"-v","error","-select_streams","v:0","-count_packets","-show_entries","stream=nb_read_packets","-of","csv=p=0",path});
    if(!probe.waitForFinished(10000) || probe.exitCode()!=0) return -1;
    bool ok=false; const int frames=probe.readAllStandardOutput().trimmed().toInt(&ok);
    return ok?frames:-1;
}
int main(int argc,char**argv) {
    media::useFfmpeg();
    QGuiApplication app(argc,argv);
    app.setOrganizationName("omacom"); app.setApplicationName("monologue-camera-check");
    Backend backend;
    bool started=false; QString lastState;
    // The preview receives every frame the camera delivers; count those that arrive while recording.
    QVideoSink preview; int delivered=0;
    backend.setPreview(&preview);
    QObject::connect(&preview,&QVideoSink::videoFrameChanged,&app,[&] { if(backend.state()=="recording") ++delivered; });
    QObject::connect(&backend,&Backend::changed,&app,[&] {
        if(backend.state()!=lastState) { lastState=backend.state(); QTextStream(stdout)<<lastState<<" · "<<backend.message()<<" · "<<backend.formatLabel()<<Qt::endl; }
        if(backend.ready() && !started) {
            started=true;
            QTimer::singleShot(200,&backend,&Backend::toggleRecording);
            QTimer::singleShot(2200,&backend,&Backend::toggleRecording);
            QTimer::singleShot(3200,&backend,&Backend::toggleRecording);
            QTimer::singleShot(5200,&backend,&Backend::finish);
        }
        if(backend.state()=="finished") {
            // The take is written at up to 30 fps, so a faster camera is not expected in full.
            const int recorded=clipFrames(backend.clip().toLocalFile());
            const int expected=std::min(delivered,qRound(backend.duration()*30));
            QTextStream(stdout)<<"Clip: "<<backend.clip().toLocalFile()<<Qt::endl
                               <<"Duration: "<<backend.duration()<<Qt::endl
                               <<"Frames: "<<recorded<<" recorded, "<<delivered<<" delivered by the camera"<<Qt::endl;
            // A take that plays for the right time can still have lost most of its frames.
            const bool keptFrames=recorded>=0.9*expected;
            if(!keptFrames) QTextStream(stdout)<<"The clip holds fewer than 90% of the camera's frames."<<Qt::endl;
            app.exit(backend.duration()>3.6 && backend.duration()<4.4 && backend.message().isEmpty() && keptFrames?0:1);
        }
        if(backend.state()=="unavailable") QTimer::singleShot(0,&app,[&] { app.exit(1); });
    });
    QTimer::singleShot(30000,&app,[&]{ qWarning()<<"Timed out:"<<backend.state()<<backend.message(); app.exit(2); });
    return app.exec();
}
