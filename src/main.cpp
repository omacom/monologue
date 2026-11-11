#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>
#include <QTimer>
#include "backend.h"
#include "theme.h"
#include "translator.h"
#include "thumbprovider.h"

int main(int argc,char *argv[]) {
    // Custom timestamped frame/buffer inputs require Qt's FFmpeg backend.
    qputenv("QT_MEDIA_BACKEND","ffmpeg");
    QGuiApplication app(argc,argv);
    app.setApplicationName("monologue");
    app.setApplicationVersion("0.3.1");
    app.setOrganizationName("omacom");
    app.setDesktopFileName("monologue");
    app.setWindowIcon(QIcon("qrc:/monologue.svg"));
    QQuickStyle::setStyle("Material");
    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);
    Theme theme;
    Backend backend;
    QQmlApplicationEngine engine;
    QObject::connect(&translator,&Translator::changed,&engine,&QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator",&translator);
    engine.rootContext()->setContextProperty("theme",&theme);
    engine.rootContext()->setContextProperty("backend",&backend);
    engine.addImageProvider("thumbs",new ThumbProvider(backend.thumbnails()->store()));
    engine.load(QUrl("qrc:/Main.qml"));
    if(engine.rootObjects().isEmpty()) return 1;
    // For headless packaging smoke checks: no fake sources or recording.
    if(app.arguments().contains("--smoke-test")) QTimer::singleShot(1200,&app,&QCoreApplication::quit);
    return app.exec();
}
