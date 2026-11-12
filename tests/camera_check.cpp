// Explicit, opt-in real-device check. Never part of bin/test.
#include <QGuiApplication>
#include <QTimer>
#include <QDebug>
#include <QTextStream>
#include <QDir>
#include <QStandardPaths>
#include "backend.h"
#include "mediautils.h"
int main(int argc,char**argv) {
    media::useFfmpeg();
    QGuiApplication app(argc,argv);
    app.setOrganizationName("omacom"); app.setApplicationName("monologue-camera-check");
    Backend backend;
    bool started=false; QString lastState;
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
            QTextStream(stdout)<<"Clip: "<<backend.clip().toLocalFile()<<Qt::endl
                               <<"Duration: "<<backend.duration()<<Qt::endl;
            app.exit(backend.duration()>3.6 && backend.duration()<4.4 && backend.message().isEmpty()?0:1);
        }
        if(backend.state()=="unavailable") QTimer::singleShot(0,&app,[&] { app.exit(1); });
    });
    QTimer::singleShot(30000,&app,[&]{ qWarning()<<"Timed out:"<<backend.state()<<backend.message(); app.exit(2); });
    return app.exec();
}
