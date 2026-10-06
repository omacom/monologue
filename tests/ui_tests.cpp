#include <QtTest>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QVideoSink>
#include <QVideoFrame>
#include "theme.h"

class UiTests : public QObject {
    Q_OBJECT
private slots:
    void keyboardAndLayout() {
        QStringList warnings;
        QQmlApplicationEngine engine;
        connect(&engine,&QQmlEngine::warnings,&engine,[&](const QList<QQmlError> &errors){ for(auto &e:errors) warnings.append(e.toString()); });
        QQmlComponent fake(&engine);
        fake.setData(R"(
import QtQml
QtObject {
    property string state: "ready"
    property string message: ""
    property string formatLabel: "3840 × 2160 · 30 fps recording"
    property var cameras: [{label: "Test camera"}]
    property var microphones: [{label: "Test microphone"}]
    property int cameraIndex: 0
    property var resolutions: [{label: "1920 × 1080 maximum"}, {label: "1280 × 720"}]
    property int resolutionIndex: 0
    property int microphoneIndex: 0
    property bool ready: state === "ready"
    property bool audioEnabled: true
    property bool takeActive: state === "recording" || state === "paused"
    property bool dialogOpen: false
    property real duration: 84
    property real level: -9
    property real peakLevel: -6
    property bool clipping: false
    property string meterText: "−9 dBFS"
    property url clip: ""
    property string clipName: "Monologue-2026-09-17.mp4"
    property real saveProgress: 0
    property var clips: [{start: 0, end: 84}]
    property var pauses: [30, 61.5]
    property real keptDuration: clips.reduce((total, c) => total + c.end - c.start, 0)
    property bool canUndo: false
    property bool canRedo: false
    property bool unsaved: true
    property int newRecordings: 0
    property QtObject thumbnails: QtObject {
        property int count: 12
        property int ready: 0
        property int revision: 0
        function request(start, end) {}
    }
    property int toggles: 0
    property int discards: 0
    signal changed()
    signal safeToClose()
    signal overwriteRequested(string path)
    function setPreview(sink) {}
    function toggleRecording() { toggles++; state = state === "ready" || state === "paused" ? "recording" : "paused"; changed() }
    function finish() { state = "finished"; changed() }
    function selectCamera(i) {}
    function selectResolution(i) {}
    function selectMicrophone(i) {}
    function newRecording() { newRecordings++; state = "ready"; changed() }
    function closeTake() {}
    function discardCurrent() { discards++; state = "ready"; changed() }
    function save() {}
    function split(t) {
        var next = []
        clips.forEach(c => { if (t > c.start && t < c.end) next.push({start: c.start, end: t}, {start: t, end: c.end}); else next.push(c) })
        clips = next; canUndo = true
    }
    function setClip(i, start, end) { var next = clips.slice(); next[i] = {start: start, end: end}; clips = next }
    function removeClip(i) { var next = clips.slice(); next.splice(i, 1); clips = next }
    function joinClips(i) { var next = clips.slice(); next[i] = {start: next[i].start, end: next[i + 1].end}; next.splice(i + 1, 1); clips = next }
    function beginGesture() {}
    function endGesture() {}
    function undo() { clips = [{start: 0, end: 84}]; canUndo = false }
    function redo() {}
    function retry() {}
}
)",QUrl());
        QScopedPointer<QObject> backend(fake.create()); QVERIFY2(backend,qPrintable(fake.errorString()));
        QTemporaryDir themeDir;
        const auto response=themeDir.filePath("rounding.json"), command=themeDir.filePath("hyprctl");
        auto write=[](const QString &path,const QByteArray &data) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data)==data.size(); };
        QVERIFY(write(command,"#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(command,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QVERIFY(write(response,"{\"int\":0}"));
        Theme theme(themeDir.path(),nullptr,command);
        engine.rootContext()->setContextProperty("backend",backend.data());
        engine.rootContext()->setContextProperty("theme",&theme);
        engine.load(QUrl("qrc:/Main.qml")); QVERIFY(!engine.rootObjects().isEmpty());
        auto *window=qobject_cast<QQuickWindow*>(engine.rootObjects().first()); QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QTest::qWait(100);
        QTest::keyClick(window,Qt::Key_Space); QCOMPARE(backend->property("state").toString(),QString("recording"));
        QTest::keyClick(window,Qt::Key_Space); QCOMPARE(backend->property("state").toString(),QString("paused"));
        QTest::keyClick(window,Qt::Key_Space); QCOMPARE(backend->property("state").toString(),QString("recording"));
        QKeyEvent repeat(QEvent::KeyPress,Qt::Key_Space,Qt::NoModifier," ",true); QCoreApplication::sendEvent(window,&repeat);
        QCOMPARE(backend->property("toggles").toInt(),3);
        backend->setProperty("dialogOpen",true); QTest::keyClick(window,Qt::Key_Space); QCOMPARE(backend->property("toggles").toInt(),3);
        backend->setProperty("dialogOpen",false);
        auto *restart=window->findChild<QObject*>("restartDialog"); QVERIFY(restart);
        QTest::keyClick(window,Qt::Key_Escape);
        QTRY_VERIFY(restart->property("opened").toBool());
        QCOMPARE(backend->property("discards").toInt(),0);
        auto *confirm=window->findChild<QQuickItem*>("restartConfirm"); QVERIFY(confirm);
        auto *cancel=window->findChild<QQuickItem*>("restartCancel"); QVERIFY(cancel);
        QVERIFY(confirm->hasActiveFocus());
        QTest::keyClick(window,Qt::Key_Tab); QVERIFY(cancel->hasActiveFocus());
        QTest::keyClick(window,Qt::Key_Backtab); QVERIFY(confirm->hasActiveFocus());
        QTest::keyClick(window,Qt::Key_Left); QVERIFY(cancel->hasActiveFocus());
        QTest::keyClick(window,Qt::Key_Right); QVERIFY(confirm->hasActiveFocus());
        QTest::keyClick(window,Qt::Key_Left);
        QTest::keyClick(window,Qt::Key_Return);
        QTRY_VERIFY(!restart->property("visible").toBool());
        QCOMPARE(backend->property("discards").toInt(),0);
        QTest::keyClick(window,Qt::Key_Escape);
        QTRY_VERIFY(restart->property("opened").toBool());
        QTest::keyClick(window,Qt::Key_Escape);
        QTRY_VERIFY(!restart->property("visible").toBool());
        QCOMPARE(backend->property("state").toString(),QString("recording"));
        for(const auto &state:{"recording","paused","finished"}) {
            backend->setProperty("state",state);
            QTest::keyClick(window,Qt::Key_Escape);
            QTRY_VERIFY(restart->property("opened").toBool());
            QVERIFY(confirm->hasActiveFocus());
            QTest::keyClick(window,Qt::Key_Return);
            QTRY_VERIFY(!restart->property("visible").toBool());
            QCOMPARE(backend->property("state").toString(),QString("ready"));
        }
        QCOMPARE(backend->property("discards").toInt(),3);
        QTest::keyClick(window,Qt::Key_Escape); QVERIFY(!restart->property("visible").toBool());
        QTest::keyClick(window,Qt::Key_Space);
        QCOMPARE(backend->property("state").toString(),QString("recording"));
        QTest::keyClick(window,Qt::Key_Return); QCOMPARE(backend->property("state").toString(),QString("finished"));
        // Split at 10 and 20 s, remove the middle clip, restore the gap with X, then trim to the playhead.
        auto *bar=window->findChild<QQuickItem*>("editBar"); QVERIFY(bar);
        auto clips=[&] { return backend->property("clips").toList(); };
        bar->setProperty("playheadSec",10); QTest::keyClick(window,Qt::Key_S);
        bar->setProperty("playheadSec",20); QTest::keyClick(window,Qt::Key_S);
        QCOMPARE(clips().size(),3);
        bar->setProperty("playheadSec",15); QTest::keyClick(window,Qt::Key_X);
        QCOMPARE(clips().size(),2); QCOMPARE(backend->property("keptDuration").toDouble(),74.0);
        window->grabWindow().save("/tmp/monologue-ui-editing.png");
        QTest::keyClick(window,Qt::Key_X); QCOMPARE(clips().size(),1);
        bar->setProperty("playheadSec",40); QTest::keyClick(window,Qt::Key_Space,Qt::ControlModifier);
        QCOMPARE(clips()[0].toMap()["start"].toDouble(),40.0);
        bar->setProperty("playheadSec",0); QTest::keyClick(window,Qt::Key_BracketRight);
        QCOMPARE(bar->property("playheadSec").toDouble(),30.0);
        // Mouse: double-click inside the clip splits it; double-click the split joins it again.
        const QPoint barOrigin=bar->mapToScene({0,0}).toPoint();
        const auto xFor=[&](double t) { return barOrigin.x()+int(bar->width()*t/84); };
        const QPoint at60(xFor(60),barOrigin.y()+int(bar->height()/2));
        QTest::mouseDClick(window,Qt::LeftButton,Qt::NoModifier,at60);
        QCOMPARE(clips().size(),2);
        const double split=clips()[0].toMap()["end"].toDouble();
        QVERIFY2(std::abs(split-60)<.5 || split==61.5,qPrintable(QString::number(split)));
        QTest::mouseDClick(window,Qt::LeftButton,Qt::NoModifier,QPoint(xFor(split)+1,at60.y()));
        QCOMPARE(clips().size(),1);
        // Double-clicking the dimmed gap before the clip restores it.
        QTest::mouseDClick(window,Qt::LeftButton,Qt::NoModifier,QPoint(xFor(20),at60.y()));
        QCOMPARE(clips()[0].toMap()["start"].toDouble(),0.0);
        backend->setProperty("state","recording");
        QTest::keyClick(window,Qt::Key_Return,Qt::ControlModifier); QCOMPARE(backend->property("state").toString(),QString("finished"));
        // Finalizing keeps the picture clear: progress shows where the timeline will appear.
        auto *overlay=window->findChild<QQuickItem*>("statusOverlay"); QVERIFY(overlay);
        auto *finishing=window->findChild<QQuickItem*>("finishingStrip"); QVERIFY(finishing);
        backend->setProperty("state","finalizing"); QTest::qWait(50);
        QVERIFY(finishing->isVisible()); QVERIFY(!overlay->isVisible());
        window->grabWindow().save("/tmp/monologue-ui-finishing.png");
        // Saving shows its progress in the Save button, not over the video.
        auto *saveButton=window->findChild<QQuickItem*>("saveButton"); QVERIFY(saveButton);
        backend->setProperty("state","saving"); backend->setProperty("saveProgress",0.42); QTest::qWait(250);
        QVERIFY(!overlay->isVisible()); QVERIFY(!finishing->isVisible());
        QCOMPARE(saveButton->property("text").toString(),QString("Saving 42%"));
        window->grabWindow().save("/tmp/monologue-ui-saving.png");
        backend->setProperty("saveProgress",0); backend->setProperty("state","finished"); QTest::qWait(50);
        QCOMPARE(saveButton->property("text").toString(),QString("Save"));
        // Leaving an unsaved take asks first; a saved one goes straight to a new recording.
        auto *unsaved=window->findChild<QObject*>("unsavedDialog"); QVERIFY(unsaved);
        auto *newButton=window->findChild<QQuickItem*>("newRecording"); QVERIFY(newButton);
        const auto newAt=newButton->mapToScene(QPointF(newButton->width()/2,newButton->height()/2)).toPoint();
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,newAt);
        QTRY_VERIFY(unsaved->property("opened").toBool());
        QCOMPARE(backend->property("newRecordings").toInt(),0);
        QTest::keyClick(window,Qt::Key_Escape); QTRY_VERIFY(!unsaved->property("visible").toBool());
        QTest::keyClick(window,Qt::Key_Q); QTRY_VERIFY(unsaved->property("opened").toBool());
        QCOMPARE(unsaved->property("action").toString(),QString("quit"));
        QTest::keyClick(window,Qt::Key_Escape); QTRY_VERIFY(!unsaved->property("visible").toBool());
        QVERIFY(window->isVisible());
        backend->setProperty("unsaved",false);
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,newAt);
        QCOMPARE(backend->property("newRecordings").toInt(),1); QVERIFY(!unsaved->property("visible").toBool());
        // Entering the editor shows the clip's first frame instead of black.
        const auto red=themeDir.filePath("red.mp4");
        QProcess encode; encode.start("ffmpeg",{"-v","error","-f","lavfi","-i","color=red:s=320x240:d=1","-c:v","libx264","-pix_fmt","yuv420p",red});
        QVERIFY(encode.waitForFinished(20000)); QCOMPARE(encode.exitCode(),0);
        auto *clipVideo=window->findChild<QObject*>("clipVideo"); QVERIFY(clipVideo);
        auto *sink=clipVideo->property("videoSink").value<QVideoSink*>(); QVERIFY(sink);
        // Reaching the end of the file must not clear the picture to black.
        QCOMPARE(clipVideo->property("endOfStreamPolicy").toInt(),1);
        QVideoFrame shown; connect(sink,&QVideoSink::videoFrameChanged,window,[&](const QVideoFrame &f) { shown=f; });
        backend->setProperty("state","ready"); QTest::qWait(50);
        backend->setProperty("clip",QUrl::fromLocalFile(red)); backend->setProperty("state","finished");
        QTRY_VERIFY_WITH_TIMEOUT(shown.isValid(),5000);
        QCOMPARE(shown.toImage().pixelColor(160,120).red()>180,true);
        window->resize(640,460); QTest::qWait(100);
        auto frame=window->grabWindow(); QVERIFY(!frame.isNull());
        frame.save("/tmp/monologue-ui-minimum.png");
        window->resize(960,700); backend->setProperty("state","ready"); QTest::qWait(100);
        window->grabWindow().save("/tmp/monologue-ui-ready.png");
        backend->setProperty("state","paused");
        QTest::keyClick(window,Qt::Key_Escape);
        QTRY_VERIFY(restart->property("opened").toBool());
        auto *dialogBackground=restart->property("background").value<QObject*>(); QVERIFY(dialogBackground);
        auto *buttonBackground=confirm->property("background").value<QObject*>(); QVERIFY(buttonBackground);
        QCOMPARE(dialogBackground->property("radius").toInt(),0);
        QCOMPARE(buttonBackground->property("radius").toInt(),0);
        QTest::qWait(50); window->grabWindow().save("/tmp/monologue-discard-square.png");
        QVERIFY(write(response,"{\"int\":6}"));
        QTRY_COMPARE(dialogBackground->property("radius").toInt(),6);
        QCOMPARE(buttonBackground->property("radius").toInt(),6);
        QTest::qWait(50); window->grabWindow().save("/tmp/monologue-discard-rounded.png");
        QVERIFY(write(response,"{\"int\":0}"));
        QTRY_COMPARE(dialogBackground->property("radius").toInt(),0);
        QCOMPARE(buttonBackground->property("radius").toInt(),0);
        delete window;
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
    }
};
int main(int argc,char**argv) {
    qputenv("QT_MEDIA_BACKEND","ffmpeg");
    QGuiApplication app(argc,argv); QQuickStyle::setStyle("Material");
    UiTests tests; return QTest::qExec(&tests,argc,argv);
}
#include "ui_tests.moc"
