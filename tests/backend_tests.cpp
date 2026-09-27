#include <QtTest>
#include <cstring>
#include <QTemporaryDir>
#include <QImage>
#include <QSignalSpy>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStandardPaths>
#include <QSettings>
#include <cmath>
#include "mediautils.h"
#include "theme.h"
#include "writer.h"
#include "backend.h"

class FakePicker : public FilePicker {
public:
    QUrl suggestion;
    void openVideo() override {}
    void exportVideo(const QUrl &url,double,double,const QList<int>&) override { suggestion=url; }
    void cancel() { emit closed(); }
    void choose(const QUrl &url) { emit closed(); emit exportSelected(url,0,0,0); }
};

class Tests : public QObject {
    Q_OBJECT
    QTemporaryDir fixture;
private slots:
    void formatSelection() {
        QList<media::Format> f{{{1920,1080},30,60,0}, {{3840,2160},15,15,0}, {{640,480},30,30,0}};
        QCOMPARE(media::bestFormat(f),1);
        f.append({{3840,2160},30,30,0}); QCOMPARE(media::bestFormat(f),3);
        f.append({{3840,2160},30,60,0}); QCOMPARE(media::bestFormat(f),3);
        QCOMPARE(media::bestFormat({}),-1);
        QCOMPARE(media::targetFps(15,15),15.0);
    }
    void clockExcludesPauses() {
        TakeClock c; c.start(1000000);
        QVERIFY(!c.position(999999));
        c.pause(6000000); QCOMPARE(c.duration(16000000),5000000);
        QVERIFY(!c.position(10000000)); c.resume(16000000);
        QCOMPARE(*c.position(16000000),5000000);
        QCOMPARE(c.duration(21000000),10000000);
        QVERIFY(!c.position(15999999));
        c.pause(21000000); c.pause(23000000); QCOMPARE(c.duration(23000000),10000000);
        QCOMPARE(*c.position(5900000),4900000); // Late delivery from a closed interval.
        const auto parts=c.spans(5900000,16100000);
        QCOMPARE(parts.size(),2);
        QCOMPARE(parts[0].position,4900000);
        QCOMPARE(parts[0].end,6000000);
        QCOMPARE(parts[1].position,5000000);
        QCOMPARE(parts[1].begin,16000000);
        QCOMPARE(c.joins(),QList<qint64>{5000000});
    }
    void editModel() {
        using namespace edit;
        auto clips=normalized({{6,7},{0,2},{1.5,4},{4.02,4.5},{9.97,12}},10);
        // Overlaps resolve, hairline gaps close, and slivers vanish.
        QCOMPARE(clips.size(),4);
        QCOMPARE(clips[0],(Range{0,2})); QCOMPARE(clips[1],(Range{2,4})); QCOMPARE(clips[2],(Range{4,4.5})); QCOMPARE(clips[3],(Range{6,7}));
        const auto parts=kept(clips);
        QCOMPARE(parts.size(),2); QCOMPARE(parts[0],(Range{0,4.5})); QCOMPARE(parts[1],(Range{6,7}));
        QCOMPARE(keptDuration(clips),5.5);
        QVERIFY(untouched(whole(10),10)); QVERIFY(untouched({{0,4},{4,10}},10)); QVERIFY(!untouched(clips,10));
        QCOMPARE(fromJson(toJson({{1,3},{5,8}}),10),(Clips{{1,3},{5,8}}));
        QCOMPARE(fromJson({},10),whole(10));
        QCOMPARE(fromJson(toJson({{4,4.05}}),10),whole(10));
        const auto args=media::exportArgs("in.mp4","out.mp4",{{0,2},{3,4.5}},true);
        QVERIFY(args.contains("[0:v:0][0:a:0][1:v:0][1:a:0]concat=n=2:v=1:a=1[v][a]"));
        QCOMPARE(args.count("-i"),2);
        QVERIFY(media::exportArgs("in.mp4","out.mp4",{{0,2}},false).contains("[0:v:0]concat=n=1:v=1:a=0[v]"));
    }
    void meterAllChannels() {
        QAudioFormat f; f.setSampleRate(48000); f.setChannelCount(2); f.setSampleFormat(QAudioFormat::Int16);
        qint16 samples[]{0,-32768,0,100};
        QCOMPARE(media::peak(QByteArray(reinterpret_cast<char*>(samples), sizeof samples),f),1.0);
        f.setSampleFormat(QAudioFormat::Float); float floats[]{0,.5f,-.75f};
        QCOMPARE(media::peak(QByteArray(reinterpret_cast<char*>(floats),sizeof floats),f),.75);
    }
    void atomicSave() {
        QTemporaryDir dir;
        QFile f(dir.filePath("original.mp4")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("kept content"); f.close();
        QVERIFY(media::copyAtomically(f.fileName(),dir.filePath("copy with spaces.mp4")).isEmpty());
        QVERIFY(!media::copyAtomically(f.fileName(),dir.filePath("missing/copy.mp4")).isEmpty());
        QVERIFY(!media::copyAtomically(f.fileName(),f.fileName()).isEmpty());
        QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(),QByteArray("kept content"));
    }
    void liveTheme() {
        QTemporaryDir d; QDir root(d.path()); root.mkpath("current"); root.mkpath("one"); root.mkpath("two");
        auto write=[](const QString &p,const QByteArray &data) { QFile f(p); if (!f.open(QIODevice::WriteOnly)) return false; return f.write(data)==data.size(); };
        QVERIFY(write(d.filePath("one/colors.toml"),"accent = '#123456' # comment\n"));
        QVERIFY(write(d.filePath("two/colors.toml"),"accent = \"#fefefe\"\n"));
        QVERIFY(QFile::link(d.filePath("one"),d.filePath("current/theme")));
        Theme theme(d.filePath("current")); QCOMPARE(theme.accent(),QString("#123456")); QCOMPARE(theme.foreground(),QString("white"));
        QVERIFY(QFile::remove(d.filePath("current/theme"))); QVERIFY(QFile::link(d.filePath("two"),d.filePath("current/theme")));
        QTRY_COMPARE(theme.accent(),QString("#fefefe")); QCOMPARE(theme.foreground(),QString("black"));
        QVERIFY(write(d.filePath("two/colors.toml"),"accent = '#333333'\n")); QTRY_COMPARE(theme.accent(),QString("#333333"));
        QVERIFY(QFile::remove(d.filePath("two/colors.toml"))); QTRY_COMPARE(theme.accent(),QString("#FFD60A"));
    }
    void liveRounding() {
        QTemporaryDir directory;
        const auto response=directory.filePath("response.json"), command=directory.filePath("hyprctl");
        auto write=[](const QString &path,const QByteArray &data) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data)==data.size(); };
        QVERIFY(write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(command,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QVERIFY(write(response,"{\"int\":0}"));
        Theme theme(directory.path(),nullptr,command);
        QCOMPARE(theme.radius(),0);
        QVERIFY(write(response,"{\"int\":6}")); QTRY_COMPARE(theme.radius(),6);
        QVERIFY(write(response,"{\"int\":0}")); QTRY_COMPARE(theme.radius(),0);
        QVERIFY(write(response,"{\"int\":8}")); QTRY_COMPARE(theme.radius(),8);
        QVERIFY(write(response,"unavailable")); QTest::qWait(1200); QCOMPARE(theme.radius(),8);
    }
    void realEncoding_data() { QTest::addColumn<bool>("sound"); QTest::newRow("silent")<<false; QTest::newRow("audio")<<true; }
    void realEncoding() {
        QFETCH(bool,sound);
        QTemporaryDir directory;
        Writer writer; QSignalSpy ended(&writer,&Writer::finished), errors(&writer,&Writer::failed);
        QAudioFormat a; if(sound) { a.setSampleRate(48000); a.setChannelCount(1); a.setSampleFormat(QAudioFormat::Int16); }
        QVideoFrameFormat format(QSize(320,240),QVideoFrameFormat::Format_BGRA8888);
        QVERIFY(Writer::supported(sound));
        QVERIFY(writer.start(directory.filePath("take.mp4"),format,30,a,0));
        // Two one-second intervals, separated by a ten-second pause in capture time.
        for(int interval=0;interval<2;++interval) {
            if(interval) {
                writer.pause(1000000);
                QImage rehearsal(320,240,QImage::Format_RGB32); rehearsal.fill(Qt::green);
                writer.video(QVideoFrame(rehearsal),5000000);
                if(sound) writer.audio(QByteArray(1600*2,char(127)),5000000);
                writer.resume(11000000);
            }
            for(int i=0;i<30;++i) {
                const qint64 t=interval*11000000LL+i*1000000LL/30;
                QImage image(320,240,QImage::Format_RGB32); image.fill(interval?Qt::blue:Qt::red);
                QVideoFrame original(image); original.setStartTime(t+123);
                writer.video(original,t);
                QCOMPARE(original.startTime(),t+123);
                if(sound && (interval || i>=6)) {
                    QByteArray bytes(1600*2,Qt::Uninitialized); auto *samples=reinterpret_cast<qint16*>(bytes.data());
                    for(int j=0;j<1600;++j) samples[j]=qint16(8000*std::sin((i*1600+j)*2*3.141592653589793*440/48000));
                    writer.audio(bytes,t);
                }
                QTest::qWait(35);
            }
        }
        writer.finish(); QTRY_VERIFY_WITH_TIMEOUT(ended.count()>0,15000);
        QVERIFY2(errors.isEmpty(),errors.isEmpty()?"":qPrintable(errors.first().first().toString()));
        const auto normalization=media::normalizeMp4(directory.filePath("take.mp4"),30);
        QVERIFY2(normalization.isEmpty(),qPrintable(normalization));
        auto p=media::probe(directory.filePath("take.mp4")); QVERIFY2(p.ok,qPrintable(p.error));
        QCOMPARE(p.size,QSize(320,240)); QCOMPARE(p.audio,sound);
        QVERIFY2(p.duration>1.8 && p.duration<2.2,qPrintable(QString::number(p.duration)));
        QProcess decode; decode.start("ffmpeg",{"-v","error","-i",directory.filePath("take.mp4"),"-f","null","-"});
        QVERIFY(decode.waitForFinished(10000)); QCOMPARE(decode.exitCode(),0);
        const auto decodeErrors=decode.readAllStandardError();
        QVERIFY2(decodeErrors.isEmpty(),decodeErrors.constData());
        QProcess streams; streams.start("ffprobe",{"-v","error","-show_streams","-of","json",directory.filePath("take.mp4")});
        QVERIFY(streams.waitForFinished());
        for(const auto &entry:QJsonDocument::fromJson(streams.readAllStandardOutput()).object()["streams"].toArray()) {
            auto stream=entry.toObject();
            const double duration=stream["duration"].toString().toDouble();
            QVERIFY2(std::abs(duration-2)<.08,qPrintable(QString::number(duration)));
            if(stream["codec_type"]=="video") QCOMPARE(stream["nb_frames"].toString().toInt(),60);
        }
        if(sound) {
            QProcess pcm; pcm.start("ffmpeg",{"-v","error","-i",directory.filePath("take.mp4"),"-vn","-ac","1","-ar","48000","-f","s16le","-"});
            QVERIFY(pcm.waitForFinished()); auto bytes=pcm.readAllStandardOutput();
            QVERIFY(bytes.size()>48000*2);
            // The initial 200 ms without microphone callbacks is real silence,
            // rather than shifting the entire audio track ahead of the video.
            QVERIFY(media::peak(bytes.left(4800*2),a)<.01);
            QVERIFY(media::peak(bytes.mid(16000*2,4800*2),a)>.1);
            QVERIFY(QFile::copy(directory.filePath("take.mp4"),fixture.filePath("fixture.mp4")));
        }
    }
    void recordingLifecycle() {
        const auto root=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/recordings";
        const QString id="00000000-0000-0000-0000-000000000001";
        const auto directory=root+"/"+id;
        QVERIFY(QDir().mkpath(directory));
        const auto original=directory+"/take.mp4";
        QVERIFY(QFile::copy(fixture.filePath("fixture.mp4"),original));
        QFile manifest(directory+"/take.json"); QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write("{\"filename\":\"take.mp4\",\"status\":\"complete\"}"); manifest.close();
        auto *picker=new FakePicker;
        Backend backend(picker,false);
        QTRY_COMPARE(backend.recordings().size(),1);
        // Recovery is nonmodal and does not open a retained take by itself.
        QVERIFY(!backend.dialogOpen()); QVERIFY(backend.clip().isEmpty());
        backend.openRecording(id); QTRY_COMPARE(backend.state(),QString("finished"));
        QCOMPARE(backend.clip().toLocalFile(),original);
        backend.save(); QVERIFY(backend.dialogOpen()); picker->cancel(); QVERIFY(!backend.dialogOpen());
        QCOMPARE(backend.state(),QString("finished")); QVERIFY(QFile::exists(original));
        const auto saved=fixture.filePath("saved clip");
        backend.save(); picker->choose(QUrl::fromLocalFile(saved));
        QTRY_COMPARE(backend.state(),QString("finished"));
        QVERIFY2(backend.message().startsWith("Saved to"),qPrintable(backend.message()));
        QVERIFY(QFile::exists(saved+".mp4")); QVERIFY(QFile::exists(original));
        QSignalSpy overwrite(&backend,&Backend::overwriteRequested);
        backend.save(); picker->choose(QUrl::fromLocalFile(saved));
        QCOMPARE(overwrite.count(),1); QVERIFY(backend.dialogOpen()); backend.confirmOverwrite(false);
        QVERIFY(!backend.dialogOpen());
        backend.save(); picker->choose(QUrl::fromLocalFile(saved)); backend.confirmOverwrite(true);
        QTRY_COMPARE(backend.state(),QString("finished")); QVERIFY(backend.message().startsWith("Saved to"));
        backend.save(); picker->choose(QUrl::fromLocalFile(fixture.filePath("missing/fail.mp4")));
        QTRY_COMPARE(backend.state(),QString("finished")); QVERIFY(backend.message().startsWith("Could not save"));
        QVERIFY(QFile::exists(original));
        backend.discardRecording("../"); QVERIFY(QFile::exists(original));
        // A fresh backend still discovers the original and remembers Save's directory.
        auto *secondPicker=new FakePicker;
        Backend reopened(secondPicker,false); QTRY_COMPARE(reopened.recordings().size(),1);
        reopened.openRecording(id); QTRY_COMPARE(reopened.state(),QString("finished"));
        reopened.save(); QCOMPARE(QFileInfo(secondPicker->suggestion.toLocalFile()).absolutePath(),fixture.path()); secondPicker->cancel();
        backend.discardCurrent(); QVERIFY(!QFile::exists(original)); QVERIFY(QFile::exists(saved+".mp4"));
        QVERIFY(backend.clip().isEmpty()); QVERIFY(!backend.takeActive());
        QCOMPARE(backend.recordings().size(),0);
    }
    void editingAndExport() {
        const auto root=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/recordings";
        const QString id="00000000-0000-0000-0000-000000000002";
        const auto directory=root+"/"+id;
        QVERIFY(QDir().mkpath(directory));
        QVERIFY(QFile::copy(fixture.filePath("fixture.mp4"),directory+"/take.mp4"));
        QFile manifest(directory+"/take.json"); QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write("{\"filename\":\"take.mp4\",\"status\":\"complete\",\"pauses\":[1.0]}"); manifest.close();
        auto *picker=new FakePicker;
        {
            Backend backend(picker,false);
            QTRY_VERIFY(!backend.recordings().isEmpty());
            backend.openRecording(id); QTRY_COMPARE(backend.state(),QString("finished"));
            QCOMPARE(backend.pauses(),QVariantList{1.0});
            QTRY_COMPARE_WITH_TIMEOUT(backend.thumbnails()->ready(),Thumbnails::Count,20000);
            QVERIFY(!backend.thumbnails()->store()->get(0).isNull() && backend.thumbnails()->store()->get(0).height()==90);
            const double d=backend.duration();
            QVERIFY(!backend.canUndo()); QCOMPARE(backend.clips().size(),1);
            auto clip=[&](int i,const char *edge) { return backend.clips()[i].toMap()[edge].toDouble(); };
            // Split, then drag the first clip's end and the second's start apart: one undo step per drag.
            backend.split(0.5); backend.split(1.5); QCOMPARE(backend.clips().size(),3);
            backend.split(1.55); QCOMPARE(backend.clips().size(),3);
            backend.removeClip(1); QCOMPARE(backend.clips().size(),2);
            QVERIFY(std::abs(backend.keptDuration()-(d-1))<.001);
            backend.undo(); QCOMPARE(backend.clips().size(),3);
            backend.beginGesture(); backend.setClip(1,0.4,1.5); backend.setClip(1,0.8,1.5); backend.endGesture();
            // Clips never overlap their neighbours.
            QCOMPARE(clip(1,"start"),0.8);
            backend.setClip(0,0,1); QCOMPARE(clip(0,"end"),0.8);
            backend.undo(); backend.undo(); QCOMPARE(clip(1,"start"),0.5); QVERIFY(backend.canUndo());
            backend.redo(); QCOMPARE(clip(1,"start"),0.8);
            backend.joinClips(0); QCOMPARE(backend.clips().size(),2); QCOMPARE(clip(0,"end"),1.5);
            backend.undo();
            // The middle clip goes: 0–0.5 and 1.5–end survive.
            backend.removeClip(1);
            backend.removeClip(0); backend.removeClip(0); QCOMPARE(backend.clips().size(),1);
            backend.undo(); QCOMPARE(backend.clips().size(),2);
            QVERIFY(std::abs(backend.keptDuration()-(d-1))<.001);
        }
        // Edits live in the manifest, so reopening a take restores them.
        Backend backend(new FakePicker,false);
        QTRY_VERIFY(!backend.recordings().isEmpty());
        backend.openRecording(id); QTRY_COMPARE(backend.state(),QString("finished"));
        QCOMPARE(backend.clips().size(),2); QVERIFY(!backend.canUndo());
        const auto kept=backend.keptDuration();
        const auto saved=fixture.filePath("edited.mp4");
        Backend exporter(picker=new FakePicker,false);
        QTRY_VERIFY(!exporter.recordings().isEmpty());
        exporter.openRecording(id); QTRY_COMPARE(exporter.state(),QString("finished"));
        exporter.save(); picker->choose(QUrl::fromLocalFile(saved));
        QCOMPARE(exporter.state(),QString("saving"));
        QTRY_COMPARE_WITH_TIMEOUT(exporter.state(),QString("finished"),30000);
        QVERIFY2(exporter.message().startsWith("Saved to"),qPrintable(exporter.message()));
        const auto p=media::probe(saved); QVERIFY2(p.ok,qPrintable(p.error));
        QVERIFY(p.audio); QCOMPARE(p.size,QSize(320,240));
        QVERIFY2(std::abs(p.duration-kept)<.1,qPrintable(QString("%1 vs %2").arg(p.duration).arg(kept)));
        // The first half second is red and what follows the cut is blue.
        QProcess pixels;
        pixels.start("ffmpeg",{"-v","error","-i",saved,"-an","-vf","scale=1:1","-pix_fmt","rgb24","-f","rawvideo","-"});
        QVERIFY(pixels.waitForFinished()); const auto rgb=pixels.readAllStandardOutput();
        QVERIFY(rgb.size()>=6);
        QVERIFY(quint8(rgb[0])>180 && quint8(rgb[2])<80);
        QVERIFY(quint8(rgb[rgb.size()-3])<80 && quint8(rgb[rgb.size()-1])>180);
        QVERIFY(!QDir(fixture.path()).entryList(QDir::Hidden|QDir::Files).join(",").contains("partial"));
        backend.discardRecording(id);
    }
    void delayedCaptureStaysInSync() {
        QTemporaryDir directory;
        const auto path=directory.filePath("sync.mp4");
        QAudioFormat format; format.setSampleRate(48000); format.setChannelCount(1); format.setSampleFormat(QAudioFormat::Int16);
        Writer writer; QSignalSpy ended(&writer,&Writer::finished), errors(&writer,&Writer::failed);
        QVERIFY(writer.start(path,QVideoFrameFormat(QSize(64,64),QVideoFrameFormat::Format_BGRA8888),30,format,0));
        QImage first(64,64,QImage::Format_RGB32); first.fill(Qt::black); writer.video(QVideoFrame(first),0);
        int nextVideo=0, nextAudio=0;
        bool paused=false,resumed=false,finished=false;
        const QList<qint64> events{300000,966667,2300000,2966667};
        for(qint64 delivered=0;delivered<=3200000;delivered+=10000) {
            if(!paused && delivered>=1000000) { writer.pause(1000000); paused=true; }
            if(!resumed && delivered>=2000000) { writer.resume(2000000); resumed=true; }
            if(!finished && delivered>=3000000) { writer.pause(3000000); finished=true; }
            // Camera callbacks are delayed by one frame; the microphone by
            // 80 ms plus the 20 ms buffer itself. Both describe the same events.
            while(qRound64(nextVideo*1000000.0/30)+33333<=delivered) {
                const qint64 captured=qRound64(nextVideo*1000000.0/30);
                QImage image(64,64,QImage::Format_RGB32);
                image.fill(events.contains(captured)?Qt::white:Qt::black);
                writer.video(QVideoFrame(image),captured); ++nextVideo;
            }
            while(nextAudio*20000LL+100000<=delivered) {
                const qint64 callbackAt=nextAudio*20000LL+100000;
                const qint64 captured=AudioCapture::captureTime(callbackAt,100000,false);
                QByteArray pcm(960*2,0); auto *samples=reinterpret_cast<qint16*>(pcm.data());
                for(int i=0;i<960;++i) {
                    const qint64 time=captured+qRound64(i*1000000.0/48000);
                    for(const auto event:events)
                        if(time>=event && time<event+10000) samples[i]=qint16(12000*std::sin(i*2*3.141592653589793*1000/48000));
                }
                writer.audio(pcm,captured); ++nextAudio;
            }
            QTest::qWait(10);
        }
        writer.finish(); QTRY_VERIFY_WITH_TIMEOUT(!ended.isEmpty(),15000);
        QVERIFY2(errors.isEmpty(),errors.isEmpty()?"":qPrintable(errors.first().first().toString()));
        QVERIFY(media::normalizeMp4(path,30).isEmpty());
        QProcess frames;
        frames.start("ffprobe",{"-v","error","-select_streams","v:0","-show_frames","-show_entries","frame=best_effort_timestamp_time","-of","json",path});
        QVERIFY(frames.waitForFinished());
        const auto timestamps=QJsonDocument::fromJson(frames.readAllStandardOutput()).object()["frames"].toArray();
        QProcess pixels;
        pixels.start("ffmpeg",{"-v","error","-i",path,"-an","-vf","scale=1:1","-pix_fmt","gray","-fps_mode","passthrough","-f","rawvideo","-"});
        QVERIFY(pixels.waitForFinished()); QCOMPARE(pixels.exitCode(),0);
        const auto values=pixels.readAllStandardOutput(); QCOMPARE(values.size(),timestamps.size());
        QList<double> flashes;
        for(int i=0;i<values.size();++i)
            if(quint8(values[i])>220) flashes.append(timestamps[i].toObject()["best_effort_timestamp_time"].toString().toDouble());
        QProcess audio;
        audio.start("ffmpeg",{"-v","error","-i",path,"-vn","-ac","1","-ar","48000","-f","s16le","-"});
        QVERIFY(audio.waitForFinished()); QCOMPARE(audio.exitCode(),0);
        const auto pcm=audio.readAllStandardOutput();
        QList<double> clicks; int lastLoud=-48000;
        for(int i=0;i<pcm.size()/2;++i) {
            qint16 sample; std::memcpy(&sample,pcm.constData()+i*2,2);
            if(std::abs(int(sample))>5000) {
                if(i-lastLoud>2400) clicks.append(i/48000.0);
                lastLoud=i;
            }
        }
        QCOMPARE(flashes.size(),4); QCOMPARE(clicks.size(),4);
        for(int i=0;i<4;++i) {
            const double expected=i<2 ? events[i]/1000000.0 : events[i]/1000000.0-1;
            QVERIFY2(std::abs(flashes[i]-expected)<.002,qPrintable(QString("Flash %1 at %2, expected %3").arg(i).arg(flashes[i]).arg(expected)));
            QVERIFY2(std::abs(flashes[i]-clicks[i])<.01,qPrintable(QString("Event %1: video %2, audio %3").arg(i).arg(flashes[i]).arg(clicks[i])));
        }
    }
};
int main(int argc,char**argv) {
    qputenv("QT_MEDIA_BACKEND","ffmpeg");
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME",config.filePath("config").toUtf8());
    qputenv("XDG_DATA_HOME",config.filePath("data").toUtf8());
    QGuiApplication app(argc,argv); app.setOrganizationName("omacom"); app.setApplicationName("monologue-tests");
    Tests tests; return QTest::qExec(&tests,argc,argv);
}
#include "backend_tests.moc"
