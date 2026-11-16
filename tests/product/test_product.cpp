// SPDX-License-Identifier: Apache-2.0
#include "core.h"
#include "companion.h"
#include "window.h"
#include "validation.h"
#include <QListWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QDialog>
#include <QTimer>
#include <QTabWidget>
#include <QComboBox>
#include <QLocalServer>
#include <csignal>
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QFile>
#include <QBuffer>
#include <QImage>
#include <QUuid>
#include <QProcess>
#include <QSignalSpy>
#include <unistd.h>
class EnvironmentGuard {
public:
    explicit EnvironmentGuard(const QList<QByteArray> &keys) {for(const auto &key:keys)values[key]=qgetenv(key);}
    ~EnvironmentGuard(){for(auto it=values.begin();it!=values.end();++it){if(it.value().isNull())qunsetenv(it.key());else qputenv(it.key(),it.value());}}
private:
    QMap<QByteArray,QByteArray> values;
};
class ProductTests: public QObject {
    Q_OBJECT
private slots:
    void initTestCase(){Q_INIT_RESOURCE(validation);Q_INIT_RESOURCE(icons);}
    void addresses(){
        QCOMPARE(WayWin::endpoint("rdp://windows.example:3390"),QString("windows.example:3390"));
        QCOMPARE(WayWin::endpoint("192.168.1.10"),QString("192.168.1.10:3389"));
        QCOMPARE(WayWin::endpoint("[::1]:3389"),QString("[::1]:3389"));
        for(const auto &s:QStringList{"rdp://user:secret@host","host/path","host?x=1","host:0","host:65536","host:","host\n/p:secret","1.2.3","999.1.1.1","-option","rdp://@host"}){
            QString error;QVERIFY2(WayWin::endpoint(s,&error).isEmpty(),qPrintable(s));QVERIFY(!error.contains("secret"));
        }
    }
    void hostContracts(){
        WayWin::Host h;h.id=QUuid::createUuid().toString();h.name="Work";h.server="host";h.user="alice";QVERIFY(h.validate().isEmpty());
        QCOMPARE(WayWin::Host::fromJson(h.json()).json(),h.json());
        auto legacy=h.json();legacy.remove("display_backend");QCOMPARE(WayWin::Host::fromJson(legacy).displayBackend,QString("auto"));
        h.displayBackend="x11";QVERIFY(h.validate().isEmpty());QCOMPARE(WayWin::Host::fromJson(h.json()).displayBackend,QString("x11"));
        h.displayBackend="offscreen";QVERIFY(!h.validate().isEmpty());h.displayBackend="auto";
        h.monitors="1,1";h.multimon=true;QVERIFY(!h.validate().isEmpty());
        h.monitors="2,1";QVERIFY(h.validate().isEmpty());
        h.user="DOMAIN\\alice";QVERIFY(!h.validate().isEmpty());
    }
    void noLaunchOnReconnect(){
        WayWin::Host h;h.server="host";h.user="alice";h.mode="rail";h.app="||Fixture";
        auto a=WayWin::arguments(h,false);QVERIFY(a.contains("/app:program:"));QVERIFY(!a.contains("/app:program:||Fixture"));
        QVERIFY(a.contains("/from-stdin:force"));QVERIFY(a.contains("-clipboard"));QVERIFY(a.contains("/sec:rdp:off"));
        QVERIFY(!a.join(' ').contains("/p:"));QVERIFY(!a.join(' ').contains("/cert:ignore"));
    }
    void environmentSanitized(){
        EnvironmentGuard guard({"FREERDP_PASSWORD","https_proxy","WLOG_FILEAPPENDER_OUTPUT_FILE_PATH","SDL_VIDEODRIVER","SDL_VIDEO_DRIVER","DISPLAY","XAUTHORITY"});
        qputenv("FREERDP_PASSWORD","secret");qputenv("https_proxy","secret");qputenv("WLOG_FILEAPPENDER_OUTPUT_FILE_PATH","secret");qputenv("SDL_VIDEODRIVER","offscreen");qputenv("SDL_VIDEO_DRIVER","dummy");qputenv("DISPLAY",":42");qputenv("XAUTHORITY","/private/auth");
        auto env=WayWin::sessionEnvironment("/private/test","x11");
        QVERIFY(!env.contains("FREERDP_PASSWORD"));QVERIFY(!env.contains("https_proxy"));QVERIFY(!env.contains("WLOG_FILEAPPENDER_OUTPUT_FILE_PATH"));
        QVERIFY(!env.contains("SDL_VIDEODRIVER"));QCOMPARE(env.value("SDL_VIDEO_DRIVER"),QString("x11"));QCOMPARE(env.value("XAUTHORITY"),QString("/private/auth"));
        QCOMPARE(WayWin::sessionEnvironment({},"offscreen").value("SDL_VIDEO_DRIVER"),QString("waywin-unavailable"));
    }
    void displayDetectionAcrossDesktops(){
        QTemporaryDir temp("/tmp/waywin-display-XXXXXX");QVERIFY(temp.isValid());
        QLocalServer server;QVERIFY(server.listen(temp.filePath("wayland-test")));
        QProcessEnvironment env;env.insert("XDG_RUNTIME_DIR",temp.path());env.insert("WAYLAND_DISPLAY","wayland-test");
        for(const auto &desktop:QStringList{"GNOME","KDE","XFCE",""}) {
            env.insert("XDG_CURRENT_DESKTOP",desktop);env.insert("XDG_SESSION_TYPE","wayland");env.insert("DISPLAY",":42");
            QCOMPARE(WayWin::displayDrivers(env),QStringList({"wayland","x11"}));
            QCOMPARE(WayWin::displayDrivers(env,"x11"),QStringList({"x11"}));
            QCOMPARE(WayWin::displayDrivers(env,"wayland"),QStringList({"wayland"}));
            env.insert("XDG_SESSION_TYPE","x11");QCOMPARE(WayWin::displayDrivers(env),QStringList({"x11","wayland"}));
        }
        env.remove("XDG_SESSION_TYPE");env.remove("XDG_CURRENT_DESKTOP");
        QCOMPARE(WayWin::displayDrivers(env),QStringList({"wayland","x11"})); // A graphical connection need not declare a desktop/session name.
        env.insert("WAYLAND_DISPLAY",temp.filePath("wayland-test"));env.remove("XDG_RUNTIME_DIR");
        QCOMPARE(WayWin::displayDrivers(env,"wayland"),QStringList({"wayland"}));
        server.close();QCOMPARE(WayWin::displayDrivers(env),QStringList({"x11"}));
        QVERIFY(WayWin::displayDrivers(env,"wayland").isEmpty());
        QFile regular(temp.filePath("ordinary"));QVERIFY(regular.open(QIODevice::WriteOnly));regular.close();
        env.insert("WAYLAND_DISPLAY",regular.fileName());QVERIFY(WayWin::displayDrivers(env,"wayland").isEmpty());
        env.remove("DISPLAY");QVERIFY(WayWin::displayDrivers(env).isEmpty());
        env.insert("WAYLAND_DISPLAY","relative");QVERIFY(WayWin::displayDrivers(env).isEmpty());
    }
    void observedDriverMustMatchSelection(){
        for(const auto &driver:QStringList{"wayland","x11","cocoa",""}) {
            QTemporaryDir temp;WayWin::Host host;WayWin::Session s(host,temp.path());s.expectedDrivers={"wayland","x11"};
            s.events.setFileName(temp.filePath("events.jsonl"));QVERIFY(s.events.open(QIODevice::WriteOnly));
            if(!driver.isEmpty())s.pending=QJsonDocument(QJsonObject{{"type","driver"},{"driver",driver}}).toJson(QJsonDocument::Compact)+'\n';
            s.pending+="{\"type\":\"connected\",\"code\":0}\n";s.receive();
            const bool valid=driver=="wayland"||driver=="x11";QCOMPARE(s.failed,!valid);
            if(valid){QCOMPARE(s.state,QString("연결됨"));QCOMPARE(s.summary()["sdl_driver_observed"].toString(),driver);}
        }
        QTemporaryDir temp;WayWin::Host host;WayWin::Session s(host,temp.path());s.expectedDrivers={"wayland"};
        s.events.setFileName(temp.filePath("events.jsonl"));QVERIFY(s.events.open(QIODevice::WriteOnly));
        s.pending="{\"type\":\"driver\",\"driver\":\"x11\"}\n";s.receive();QVERIFY(s.failed);
    }
    void preconnectFailureExplained(){
        for(const auto code:{131073,131081}) {
            QTemporaryDir temp;WayWin::Host host;WayWin::Session s(host,temp.path());
            s.events.setFileName(temp.filePath("events.jsonl"));QVERIFY(s.events.open(QIODevice::WriteOnly));
            s.pending=QJsonDocument(QJsonObject{{"type","disconnected"},{"code",code}}).toJson(QJsonDocument::Compact)+'\n';s.receive();
            QVERIFY(s.detail.contains(QString::number(code)));
            if(code==131073) {
                QVERIFY(s.detail.contains("서버 접속 전 초기화"));
                QVERIFY(s.detail.contains("ERRCONNECT_PRE_CONNECT_FAILED"));
            } else QVERIFY(s.detail.contains("인증"));
            const auto message=s.detail;
            s.finish(136,QProcess::NormalExit);
            QCOMPARE(s.state,QString("연결 실패"));QCOMPARE(s.detail,message);
        }
    }
    void privateAtomicStorage(){
        QTemporaryDir temp;QString e;auto file=temp.filePath("settings.json");
        QVERIFY(WayWin::saveJson(file,{{"value",7}},&e));QCOMPARE(WayWin::readJson(file)["value"].toInt(),7);
        QVERIFY(!(QFile::permissions(file)&(QFile::ReadGroup|QFile::ReadOther|QFile::WriteGroup|QFile::WriteOther)));
        QVERIFY(WayWin::saveJson(file,{{"value",8}},&e));QCOMPARE(WayWin::readJson(file)["value"].toInt(),8);
        QVERIFY(!WayWin::saveJson(temp.filePath("missing/file"),{},&e));QCOMPARE(WayWin::readJson(file)["value"].toInt(),8);
    }
    void boundedEventPrivacy(){
        WayWin::EventDecoder d;
        QVERIFY(d.feed("secret password\n").isEmpty());
        QVERIFY(d.feed("WAYWIN_PROBE_V1 driver=way").isEmpty());auto e=d.feed("land\n");QCOMPARE(e.size(),1);QCOMPARE(e[0]["driver"].toString(),QString("wayland"));
        QVERIFY(d.feed(QByteArray(100000,'x')+"WAYWIN_PROBE_V1 driver=wayland\n").isEmpty());
        QCOMPARE(d.feed("WAYWIN_PRODUCT_V1 connected code=0\n").size(),1);
        QVERIFY(d.feed("WAYWIN_PROBE_V1 type=window-create window=4294967296 icon=0 flags=0\n").isEmpty());
        QVERIFY(d.feed("WAYWIN_PROBE_V1 type=title window=1 icon=0 flags=0\n").isEmpty());
    }
    void reportRequirements(){
        auto r=WayWin::newReport();QCOMPARE(r["schema"].toInt(),3);QCOMPARE(r["cases"].toObject().size(),10);QCOMPARE(r["monitor_matrix"].toObject().size(),6);
        QVERIFY(!WayWin::assessReport(r).isEmpty());
        r["tester"]="Tester";r["date"]="2026-11-14";auto env=r["environment"].toObject();for(auto it=env.begin();it!=env.end();++it)it.value()="recorded";env["sdl_driver"]="wayland";r["environment"]=env;
        QJsonArray apps;for(int i=0;i<5;++i)apps.append(QJsonObject{{"name",QString::number(i)},{"version","1"}});r["applications"]=apps;
        for(auto group:QStringList{"cases","shell_surfaces","monitor_matrix"}){
            auto items=r[group].toObject();for(auto it=items.begin();it!=items.end();++it){auto item=it.value().toObject();item["status"]="pass";item["notes"]="observed";item["evidence"]=QJsonArray{"runs/example"};
                if(group=="shell_surfaces")for(auto f:QStringList{"remote_ui_visible","interactive","same_session"})item[f]=true;
                if(group=="monitor_matrix")for(auto f:QStringList{"host_physical_layout","client_layout","rdp_session_layout"})item[f]="recorded";
                it.value()=item;
            }r[group]=items;
        }
        QVERIFY(WayWin::assessReport(r).isEmpty());
        env["sdl_driver"]="x11";env["desktop"]="KDE Plasma";r["environment"]=env;QVERIFY(WayWin::assessReport(r).isEmpty());
        env["sdl_driver"]="offscreen";r["environment"]=env;QVERIFY(!WayWin::assessReport(r).isEmpty());
        env["sdl_driver"]="x11";r["environment"]=env;
        auto cases=r["cases"].toObject();cases.remove("P09");r["cases"]=cases;QVERIFY(!WayWin::assessReport(r).isEmpty());
    }
    void legacyReportMigration(){
        auto old=WayWin::newReport();old["schema"]=2;old["environment"]=QJsonObject{{"ubuntu","26.04"},{"gnome","50"},{"sdl_driver","wayland"}};
        old["independently_verified"]=true;
        auto r=WayWin::migrateReport(old);QCOMPARE(r["schema"].toInt(),3);QCOMPARE(r["cases"],old["cases"]);
        const auto env=r["environment"].toObject();QCOMPARE(env["distribution"].toString(),QString("Ubuntu 26.04"));QCOMPARE(env["desktop"].toString(),QString("GNOME 50"));
        QCOMPARE(env["session_type"].toString(),QString("wayland"));QVERIFY(env["execution_environment"].toString().isEmpty());QVERIFY(!r["independently_verified"].toBool());
        QCOMPARE(WayWin::migrateReport(r),r);old["schema"]=99;QVERIFY(WayWin::migrateReport(old).isEmpty());
    }
    void hostGuiPersistence(){
        QTemporaryDir temp;
        {
            WayWin::Window window(temp.path());window.show();
            auto add=window.findChild<QPushButton*>("addHost");QVERIFY(add);
            QTimer::singleShot(0,&window,[&]{
                auto dialog=window.findChild<QDialog*>();QVERIFY(dialog);
                dialog->findChild<QLineEdit*>("name")->setText("Validation host");
                dialog->findChild<QLineEdit*>("server")->setText("windows.example");
                dialog->findChild<QLineEdit*>("user")->setText("tester");
                auto backend=dialog->findChild<QComboBox*>("displayBackend");QVERIFY(backend);backend->setCurrentIndex(backend->findData("x11"));
                if(qEnvironmentVariableIsSet("WAYWIN_TEST_ARTIFACTS")){dialog->adjustSize();QCoreApplication::processEvents();dialog->grab().save(qEnvironmentVariable("WAYWIN_TEST_ARTIFACTS")+"/host-settings.png");}
                dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
            });add->click();
            QCOMPARE(window.findChild<QListWidget*>("hostList")->count(),1);
            QVERIFY(QFileInfo::exists(temp.filePath("hosts.json")));
            QCOMPARE(WayWin::readJson(temp.filePath("hosts.json"))["hosts"].toArray()[0].toObject()["display_backend"].toString(),QString("x11"));
            auto v=window.findChild<WayWin::Validation*>();QVERIFY(v);QVERIFY(v->isEnabled());
            v->attachRun({{"run_id","test-run"},{"sdl_driver_observed","x11"}});
            QCOMPARE(v->report()["environment"].toObject()["sdl_driver"].toString(),QString("x11"));
        }
        WayWin::Window reopened(temp.path());
        if(qEnvironmentVariableIsSet("WAYWIN_TEST_ARTIFACTS")){reopened.show();auto tabs=reopened.findChild<QTabWidget*>();for(int i=0;i<tabs->count();++i){tabs->setCurrentIndex(i);QCoreApplication::processEvents();reopened.grab().save(qEnvironmentVariable("WAYWIN_TEST_ARTIFACTS")+QString("/tab-%1.png").arg(i));}}
        QCOMPARE(reopened.findChild<QListWidget*>("hostList")->count(),1);
        auto v=reopened.findChild<WayWin::Validation*>();QCOMPARE(v->report()["environment"].toObject()["sdl_driver"].toString(),QString("x11"));
        v->attachRun({{"run_id","unobserved-run"}});QVERIFY(v->report()["environment"].toObject()["sdl_driver"].toString().isEmpty());
    }
    void workerIsolationAndCleanup(){
        QProcess first,second;
        const QString program=QStringLiteral(WAYWIN_EXECUTABLE);
        const QString script="read credential; printf 'raw-sensitive-output\\n' >&2; printf 'WAYWIN_PRODUCT_V1 connected code=0\\n' >&2; sleep 60";
        auto start=[&](QProcess &p){p.start(program,{"--session-worker","/bin/sh","-c",script});QVERIFY(p.waitForStarted());p.write("synthetic-password\n");};
        start(first);start(second);
        QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output+=first.readAllStandardOutput()).contains("connected"),5000);
        QVERIFY(!output.contains("sensitive"));QVERIFY(!output.contains("password"));
        qint64 child=0;for(const auto &line:output.split('\n')){auto o=QJsonDocument::fromJson(line).object();if(o["type"].toString()=="process")child=qint64(o["pid"].toDouble());}
        QVERIFY(child>0);::kill(-child,SIGSTOP);
        first.closeWriteChannel();QVERIFY(first.waitForFinished(5000));
        QVERIFY(second.state()==QProcess::Running);
        QTRY_VERIFY_WITH_TIMEOUT(::kill(child,0)!=0,3000);
        second.closeWriteChannel();QVERIFY(second.waitForFinished(5000));
    }
    void workerDoesNotStartBeforeCredentials(){
        QTemporaryDir temp;const auto file=temp.filePath("started");QProcess process;
        process.start(QStringLiteral(WAYWIN_EXECUTABLE),{"--session-worker","/usr/bin/touch",file});QVERIFY(process.waitForStarted());process.closeWriteChannel();QVERIFY(process.waitForFinished(3000));QVERIFY(!QFileInfo::exists(file));
    }
    void runtimeIntegrityAndContainment(){
        QTemporaryDir temp;QDir().mkpath(temp.filePath("bin"));
        const auto binary=temp.filePath("bin/engine");QFile file(binary);QVERIFY(file.open(QIODevice::WriteOnly));file.write("fixture");file.close();QFile::setPermissions(binary,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
        const auto hash=QString::fromLatin1(QCryptographicHash::hash("fixture",QCryptographicHash::Sha256).toHex());
        QJsonObject m{{"schema","waywin-runtime-1"},{"source_commit","65bcca1913a28cbdd7eb913599f692bd2b707991"},{"product_patch_sha256",WAYWIN_TEST_PRODUCT_PATCH},{"probe_patch_sha256",WAYWIN_TEST_PROBE_PATCH},{"binary","bin/engine"},{"files",QJsonObject{{"bin/engine",hash}}}};
        const auto manifest=temp.filePath("waywin-runtime.json");QVERIFY(WayWin::saveJson(manifest,m));QString error;
        QCOMPARE(WayWin::verifyRuntime(manifest,&error),binary);
        m["system"]="Linux";QVERIFY(WayWin::saveJson(manifest,m));QVERIFY(WayWin::verifyRuntime(manifest,&error).isEmpty());
        m["display_backends"]=QJsonArray{"wayland","x11"};QVERIFY(WayWin::saveJson(manifest,m));QCOMPARE(WayWin::verifyRuntime(manifest,&error),binary);
        QVERIFY(file.open(QIODevice::WriteOnly));file.write("changed");file.close();QVERIFY(WayWin::verifyRuntime(manifest,&error).isEmpty());
        QVERIFY(file.open(QIODevice::WriteOnly));file.write("fixture");file.close();
        QFile extra(temp.filePath("libextra.so"));QVERIFY(extra.open(QIODevice::WriteOnly));extra.write("unlisted");extra.close();QVERIFY(WayWin::verifyRuntime(manifest,&error).isEmpty());extra.remove();
        QTemporaryDir outside;QFile external(outside.filePath("engine"));QVERIFY(external.open(QIODevice::WriteOnly));external.write("fixture");external.close();QFile::remove(binary);QVERIFY(QFile::link(external.fileName(),binary));QVERIFY(WayWin::verifyRuntime(manifest,&error).isEmpty());
    }


    void surfaceWorkersTerminateOnEof(){
        using namespace WayWinWire;
        QImage image(40,30,QImage::Format_RGB32);image.fill(Qt::blue);QByteArray jpeg;QBuffer buffer(&jpeg);buffer.open(QIODevice::WriteOnly);QVERIFY(image.save(&buffer,"JPEG"));
        Message frame{Frame,1,10,{}};put(frame.data,1);put(frame.data,40);put(frame.data,30);
        frame.data.insert(frame.data.end(),reinterpret_cast<const uint8_t*>(jpeg.constData()),reinterpret_cast<const uint8_t*>(jpeg.constData()+jpeg.size()));
        auto bytes=encode(frame);QProcess a,b;
        for(auto p:{&a,&b}){p->start(QStringLiteral(WAYWIN_EXECUTABLE),{"--surface-worker","Test host"});QVERIFY(p->waitForStarted());p->write(reinterpret_cast<const char*>(bytes.data()),qsizetype(bytes.size()));}
        QTest::qWait(200);
        a.closeWriteChannel();QVERIFY(a.waitForFinished(5000));QCOMPARE(a.exitCode(),0);
        QCOMPARE(b.state(),QProcess::Running);const auto output=a.readAllStandardOutput();
        b.closeWriteChannel();QVERIFY(b.waitForFinished(5000));QCOMPARE(b.exitCode(),0);
        Decoder decoder;auto commands=decoder.feed(reinterpret_cast<const uint8_t*>(output.constData()),size_t(output.size()));
        for(const auto&message:commands)QVERIFY(message.type==Pointer||message.type==Key||message.type==SurfaceClose||message.type==SurfaceState);
        QVERIFY(!output.contains(jpeg)); // The worker returns commands, never the image payload.
    }
    void binaryProtocolBoundaries(){
        using namespace WayWinWire;
        Message input{Clipboard,42,9,{}};string(input.data,"private clipboard");auto bytes=encode(input);
        Decoder decoder;std::vector<Message> messages;
        for(auto b:bytes){auto part=decoder.feed(&b,1);messages.insert(messages.end(),part.begin(),part.end());}
        QCOMPARE(messages.size(),size_t(1));QCOMPARE(messages[0].request,42u);QCOMPARE(messages[0].epoch,9u);
        Reader read(messages[0].data);QCOMPARE(read.text(),std::string("private clipboard"));QVERIFY(read.done());
        QCOMPARE(unhex(hex(bytes)),bytes);
        auto bad=bytes;bad[4]=3;QVERIFY_THROWS_EXCEPTION(std::runtime_error,decoder.feed(bad.data(),bad.size()));
        bad=bytes;bad[20]=0xff;bad[21]=0xff;bad[22]=0xff;QVERIFY_THROWS_EXCEPTION(std::runtime_error,decoder.feed(bad.data(),bad.size()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,unhex("01xz"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,unhex("1"));
        std::vector<uint8_t> empty;Reader truncated(empty);QVERIFY_THROWS_EXCEPTION(std::runtime_error,truncated.number());
    }
    void transientDataIsNotEvidence(){
        using namespace WayWinWire;
        QTemporaryDir temp;WayWin::Host host;host.id="host-a";host.name="A";WayWin::Session session(host,temp.path());
        session.events.setFileName(temp.filePath("events.jsonl"));QVERIFY(session.events.open(QIODevice::WriteOnly));
        session.runId="run-a";session.windows.insert(77);session.icons.insert(quint64(77)<<32);
        Message title{Title,77,1,{}};put(title.data,0);string(title.data,"CONFIDENTIAL_DOCUMENT");
        auto bytes=encode(title);auto line=QByteArray::fromStdString("WAYWIN_UI_V2 "+hex(bytes)+"\n");
        WayWin::EventDecoder filter;auto event=filter.feed(line);QCOMPARE(event.size(),1);QCOMPARE(event[0]["type"].toString(),QString("ui"));
        session.receiveTransient(QByteArray(reinterpret_cast<const char*>(bytes.data()),qsizetype(bytes.size())));
        QCOMPARE(session.windowTitles.value(77),QString("CONFIDENTIAL_DOCUMENT"));
        title.type=TrayTitle;bytes=encode(title);session.receiveTransient(QByteArray(reinterpret_cast<const char*>(bytes.data()),qsizetype(bytes.size())));
        QCOMPARE(session.trayTitles.value(quint64(77)<<32),QString("CONFIDENTIAL_DOCUMENT")); // Icon ID zero is valid.
        title.type=Title;
        Message recovery{Retry,0,2,{}};for(auto n:{1,2,4,0})put(recovery.data,uint32_t(n));bytes=encode(recovery);
        session.receiveTransient(QByteArray(reinterpret_cast<const char*>(bytes.data()),qsizetype(bytes.size())));
        QCOMPARE(session.state,QString("재연결 중"));QCOMPARE(session.epoch,2u);
        title.request=78;bytes=encode(title);session.receiveTransient(QByteArray(reinterpret_cast<const char*>(bytes.data()),qsizetype(bytes.size())));
        QVERIFY(!session.windowTitles.contains(78)); // Previous generation is discarded.
        session.events.close();QFile log(temp.filePath("events.jsonl"));QVERIFY(log.open(QIODevice::ReadOnly));auto evidence=log.readAll();
        QVERIFY(evidence.contains("recovery"));QVERIFY(!evidence.contains("CONFIDENTIAL"));QVERIFY(!evidence.contains("payload"));
    }
    void hostScopedCompanionAndClipboard(){
        using namespace WayWinWire;
        QTemporaryDir temp;WayWin::Host host;host.id="a";host.name="A";WayWin::Session a(host,temp.path());host.id="b";WayWin::Session b(host,temp.path());
        QSignalSpy aMessages(&a,&WayWin::Session::transient),bMessages(&b,&WayWin::Session::transient);
        Message clip{Clipboard,30,1,{}};string(clip.data,"host A only");
        auto bytes=encode(clip);auto data=QByteArray(reinterpret_cast<const char*>(bytes.data()),qsizetype(bytes.size()));
        a.receiveTransient(data);QCOMPARE(aMessages.size(),0); // Unsolicited clipboard is rejected.
        a.requests[30]=QDateTime::currentMSecsSinceEpoch()+1000;
        a.receiveTransient(data);QCOMPARE(aMessages.size(),1);QCOMPARE(bMessages.size(),0);QVERIFY(!b.companionReady);
        Message ready{Ready,1,1,{}};put(ready.data,4);string(ready.data,"instance-a");put(ready.data,0x3f);bytes=encode(ready);
        a.receiveTransient(QByteArray(reinterpret_cast<const char*>(bytes.data()),qsizetype(bytes.size())));
        QVERIFY(a.companionReady);QCOMPARE(a.companionIdentity,QString("4:instance-a"));
        ready.data.clear();put(ready.data,5);string(ready.data,"instance-b");put(ready.data,0x3f);ready.epoch=2;bytes=encode(ready);
        a.receiveTransient(QByteArray(reinterpret_cast<const char*>(bytes.data()),qsizetype(bytes.size())));
        QVERIFY(a.detail.contains("변경"));QVERIFY(b.companionIdentity.isEmpty());
        WayWin::Companion panel;panel.setSession(&a);panel.show();
        Message appList{AppList,31,2,{}};put(appList.data,1);put(appList.data,7);string(appList.data,"Remote editor");
        a.requests[31]=QDateTime::currentMSecsSinceEpoch()+1000;bytes=encode(appList);a.receiveTransient(QByteArray(reinterpret_cast<const char*>(bytes.data()),qsizetype(bytes.size())));
        QCOMPARE(panel.findChild<QListWidget*>("remoteApps")->count(),1);
        panel.setSession(&b);QCOMPARE(panel.findChild<QListWidget*>("remoteApps")->count(),0);
    }

    void retryBudgetWithFakeClock(){
        using namespace WayWinPolicy;
        RetryBudget retry;retry.disconnected(1000,1000);QVERIFY(!retry.begin(1999));
        retry.stop();for(int i=0;i<50;++i)retry.wake(2000+i);QCOMPARE(retry.state,RetryState::Stopped);QVERIFY(!retry.begin(3000));
        retry.manual(3000);
        for(unsigned n=1;n<=5;++n){retry.wake(3000+n);QVERIFY(retry.begin(3000+n));QCOMPARE(retry.attempts,n);retry.failed(3000+n,10000);}
        QCOMPARE(retry.state,RetryState::Exhausted);
        for(int i=0;i<50;++i){retry.wake(100000+i);QVERIFY(!retry.begin(100000+i));}QCOMPARE(retry.attempts,5u);
        retry.manual(200000);QVERIFY(retry.begin(200000));QCOMPARE(retry.attempts,1u);
        retry.succeeded();retry.disconnected(300000,0);QVERIFY(retry.begin(300000));QCOMPARE(retry.attempts,1u);
        retry.forbid();retry.wake(500000);retry.manual(500000);QVERIFY(!retry.begin(500000));
    }
    void boundedQueueAndInputOrdering(){
        using namespace WayWinWire;using namespace WayWinPolicy;
        Queue queue;
        auto pointer=[](uint32_t request,uint32_t action,uint32_t target=7){Message m{Pointer,request,4,{}};for(auto n:{target,100u,200u,action})put(m.data,n);return packet(std::to_string(request),m);};
        QCOMPARE(queue.push(pointer(1,0)),Admission::Accepted);queue.push(pointer(2,0));QCOMPARE(queue.size(),size_t(1));QCOMPARE(queue.replacedRequest,1u);
        queue.push(pointer(3,1));queue.push(pointer(4,0));queue.push(pointer(5,0));queue.push(pointer(6,2));
        std::vector<uint32_t> ordered;while(!queue.empty()){ordered.push_back(queue.front().request);queue.pop();}
        QCOMPARE(ordered,std::vector<uint32_t>({2,3,5,6})); // A release never passes the down event or intervening motion.
        for(size_t i=0;i<NormalCommands;++i)QCOMPARE(queue.push({"normal"}),Admission::Accepted);
        QCOMPARE(queue.push({"extra"}),Admission::Full);
        for(size_t i=0;i<ControlCommands;++i)QCOMPARE(queue.push({"release",true}),Admission::Accepted);
        QCOMPARE(queue.push({"overflow",true}),Admission::ControlFull);QCOMPARE(queue.size(),NormalCommands+ControlCommands);
        queue.clear();QCOMPARE(queue.push({std::string(NormalBytes,'x')}),Admission::Accepted);QCOMPARE(queue.push({"x"}),Admission::Full);
        QCOMPARE(queue.push({std::string(ControlBytes,'x'),true}),Admission::Accepted);QCOMPARE(queue.bytes(),NormalBytes+ControlBytes);
        QCOMPARE(queue.push({"x",true}),Admission::ControlFull);queue.discardPending(false);QVERIFY(queue.low());QCOMPARE(queue.bytes(),size_t(0));
        queue.push(pointer(10,0));queue.push(pointer(11,0),true);QCOMPARE(queue.size(),size_t(2)); // Never rewrite a partially transmitted command.
        queue.clear();auto old=pointer(12,0),fresh=pointer(13,0);fresh.epoch=5;queue.push(old);queue.push(fresh);QCOMPARE(queue.size(),size_t(2));
        queue.discardPending(true);QCOMPARE(queue.size(),size_t(1));
        Message key{Key,20,5,{}};put(key.data,7);put(key.data,16);put(key.data,0);
        auto wire=encode(key);auto command="20 0 0 5 "+hex(wire);
        auto parsed=commandPacket(command);QVERIFY(parsed.control);QCOMPARE(parsed.epoch,5u);QCOMPARE(parsed.request,20u);
        QCOMPARE(commandPacket("1 77 0 5 21").request,21u);QVERIFY(commandPacket("4 77 0 5 22").control);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,commandPacket("1 77 0 5 4294967296"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,commandPacket("20 0 0 5 invalid"));
        Stall stall;stall.set(true,100);QVERIFY(!stall.expired(5099));QVERIFY(stall.expired(5100));stall.set(false,5100);QVERIFY(!stall.expired(9000));
    }
    void captureScheduleWithFakeClock(){
        using namespace WayWinWire;using namespace WayWinPolicy;
        CaptureScheduler scheduler;scheduler.add(1);scheduler.add(2);scheduler.add(3);scheduler.state(1,Active);
        for(uint64_t t=0;t<2000;t+=250)QVERIFY(scheduler.take(t,true)); // Consume initial frames.
        std::map<uint32_t,unsigned> frames;
        for(uint64_t t=2000;t<6000;t+=250){auto id=scheduler.take(t,true);QVERIFY(id);++frames[id];QCOMPARE(scheduler.take(t,true),0u);}
        QCOMPARE(frames[1],12u);QCOMPARE(frames[2],2u);QCOMPARE(frames[3],2u);
        scheduler.state(2,Active);frames.clear();for(uint64_t t=6000;t<10000;t+=250)++frames[scheduler.take(t,true)];QCOMPARE(frames[2],12u);QCOMPARE(frames[1]+frames[3],4u);
        scheduler.state(2,Hidden);scheduler.state(1,Hidden);scheduler.state(3,Hidden);QCOMPARE(scheduler.take(10000,true),0u);
        scheduler.state(3,Background);QCOMPARE(scheduler.take(10000,true),3u);QCOMPARE(scheduler.take(10250,true),0u);QCOMPARE(scheduler.take(11000,true),3u);
        scheduler.add(4);QCOMPARE(scheduler.take(12000,true),4u); // New first frame wins the next available slot.
        scheduler.state(3,Hidden);scheduler.state(4,Active);frames.clear();for(uint64_t t=13000;t<14000;t+=250)++frames[scheduler.take(t,true)];QCOMPARE(frames[4],4u);
        QCOMPARE(scheduler.take(60000,true),4u);QCOMPARE(scheduler.take(60000,true),0u); // No catch-up burst.
        scheduler.clear();for(uint32_t i=1;i<=8;++i)scheduler.add(i);
        frames.clear();for(uint64_t t=0;t<8000;t+=250)++frames[scheduler.take(t,false)];for(uint32_t i=1;i<=8;++i)QCOMPARE(frames[i],4u);
        CaptureDeadline deadline;QVERIFY(deadline.start(0));QVERIFY(!deadline.start(1));QVERIFY(!deadline.expired(4999));QVERIFY(deadline.expired(5000));deadline.fault();QVERIFY(!deadline.available());QVERIFY(!deadline.start(9000));deadline.reopen();QVERIFY(deadline.start(9000));deadline.complete();QVERIFY(deadline.available());
    }
    void cacheLifetimesAndLimits(){
        using namespace WayWinWire;
        QTemporaryDir temp;WayWin::Host host;WayWin::Session s(host,temp.path());
        s.events.setFileName(temp.filePath("events"));QVERIFY(s.events.open(QIODevice::WriteOnly));
        auto event=[&](const char*type,uint32_t window,uint32_t icon=0){s.pending=QJsonDocument(QJsonObject{{"type",type},{"window",double(window)},{"icon",double(icon)}}).toJson(QJsonDocument::Compact)+'\n';s.receive();};
        auto deliver=[&](Message m){auto b=encode(m);s.receiveTransient(QByteArray(reinterpret_cast<const char*>(b.data()),qsizetype(b.size())));};
        auto title=[&](uint32_t type,uint32_t id,uint32_t icon){Message m{type,id,s.epoch,{}};put(m.data,icon);string(m.data,"PRIVATE");deliver(m);};
        auto image=[&](uint32_t id){Message m{Icon,10,s.epoch,{}};put(m.data,id);put(m.data,256);put(m.data,256);m.data.resize(12+256*256*4,1);deliver(m);};
        title(Title,10,0);title(TrayTitle,10,0);image(0);QVERIFY(s.windowTitles.isEmpty());QVERIFY(s.trayTitles.isEmpty());QVERIFY(s.trayImages.isEmpty());
        for(int n=0;n<100;++n){event("window-create",10);event("notify-create",10,0);title(Title,10,0);title(TrayTitle,10,0);image(0);event("window-delete",10);QVERIFY(s.windowTitles.isEmpty());QVERIFY(s.trayTitles.isEmpty());QVERIFY(s.trayImages.isEmpty());QVERIFY(s.icons.isEmpty());QCOMPARE(s.imageBytes,qsizetype(0));}
        event("window-create",10);
        for(uint32_t i=0;i<63;++i){event("notify-create",10,i);image(i);}
        const auto first=quint64(10)<<32;QVERIFY(!s.trayImage(first).isEmpty());
        event("notify-create",10,63);image(63);QVERIFY(s.trayImages.contains(first));QVERIFY(!s.trayImages.contains(first|1));
        QVERIFY(s.imageBytes<=qsizetype(WayWinPolicy::ImageBytes));QCOMPARE(s.trayImages.size(),63);
        title(TrayTitle,10,0);event("notify-delete",10,0);QVERIFY(!s.trayTitles.contains(first));QVERIFY(!s.trayImages.contains(first));
        s.requests[20]=100;s.inputRequests.insert(20);s.controlRequests.insert(21);s.inputDeadlines[20]=100;
        Message retry{Retry,0,2,{}};for(auto n:{1u,1u,1u,0u})put(retry.data,n);deliver(retry);
        QVERIFY(s.windows.contains(10));QVERIFY(s.windowTitles.isEmpty());QVERIFY(s.icons.isEmpty());QVERIFY(s.trayImages.isEmpty());QVERIFY(s.requests.isEmpty());QVERIFY(s.inputRequests.isEmpty());QVERIFY(s.controlRequests.isEmpty());QVERIFY(s.inputDeadlines.isEmpty());
        for(uint32_t i=0;i<WayWinPolicy::IdLimit;++i)event("window-create",100+i);
        QVERIFY(s.failed);QVERIFY(s.windows.size()<=int(WayWinPolicy::IdLimit));
        s.failed=false;s.windows.clear();for(uint32_t i=0;i<=WayWinPolicy::IdLimit;++i)event("notify-create",20,i);
        QVERIFY(s.failed);QCOMPARE(s.icons.size(),int(WayWinPolicy::IdLimit));
        s.finish(0,QProcess::NormalExit);QVERIFY(s.windows.isEmpty());QVERIFY(s.icons.isEmpty());QVERIFY(s.windowTitles.isEmpty());QVERIFY(s.trayTitles.isEmpty());
    }
    void inputCompletionAndOldCompanion(){
        using namespace WayWinWire;
        QTemporaryDir temp;WayWin::Host host;WayWin::Session s(host,temp.path());
        auto deliver=[&](Message m){auto b=encode(m);s.receiveTransient(QByteArray(reinterpret_cast<const char*>(b.data()),qsizetype(b.size())));};
        Message ready{Ready,1,1,{}};put(ready.data,1);string(ready.data,"test");put(ready.data,0x3f);deliver(ready);
        QVERIFY(s.companionReady);QVERIFY(!s.shellReady);QVERIFY(s.companionStatus.contains("업데이트"));
        s.inputRequests.insert(20);s.requests[20]=100;s.inputDeadlines[20]=100;
        QSignalSpy changed(&s,&WayWin::Session::changed),companion(&s,&WayWin::Session::companionChanged);
        auto previous=s.companionStatus;deliver({InputAck,20,1,{}});QVERIFY(s.inputRequests.isEmpty());QVERIFY(s.requests.isEmpty());QVERIFY(s.inputDeadlines.isEmpty());QCOMPARE(changed.size(),0);QCOMPARE(companion.size(),0);QCOMPARE(s.companionStatus,previous);
        Message flow{Flow,0,1,{}};put(flow.data,2);put(flow.data,1);deliver(flow);QVERIFY(s.inputBlocked);
        s.requests[21]=100;s.inputRequests.insert(21);deliver({InputAck,21,1,{}});QVERIFY(s.inputBlocked); // Acknowledgment cannot clear a blocked downstream stage.
        Message load{Flow,0,1,{}};put(load.data,1);put(load.data,3);deliver(load);
        flow.data.clear();put(flow.data,2);put(flow.data,0);deliver(flow);QVERIFY(s.inputBlocked); // Another stage still exceeds the resume watermark.
        load.data.clear();put(load.data,1);put(load.data,0);deliver(load);QVERIFY(!s.inputBlocked);
        s.state="연결됨";WayWin::Companion panel;panel.setSession(&s);panel.show();QVERIFY(panel.findChild<QListWidget*>("remoteApps")->isEnabled());
        if(qEnvironmentVariableIsSet("WAYWIN_TEST_ARTIFACTS")){panel.adjustSize();QCoreApplication::processEvents();panel.grab().save(qEnvironmentVariable("WAYWIN_TEST_ARTIFACTS")+"/companion-update.png");}
        Message gone{SurfaceGone,99,1,{}};string(gone.data,"Synthetic capture failure before first frame");deliver(gone);QVERIFY(s.companionStatus.contains("before first frame"));
    }
    void surfaceVisibilityEventsAndHiddenFrames(){
        using namespace WayWinWire;
        int pipe[2];QVERIFY(::pipe(pipe)==0);::fcntl(pipe[0],F_SETFL,O_NONBLOCK);
        WayWin::Host host;host.name="Synthetic host";WayWin::Session s(host,{});
        s.surfaceProxy=s.companionReady=s.shellReady=true;s.state="연결됨";s.epoch=10;
        s.proxyWriter=new WayWin::CommandWriter(pipe[1],&s);
        WayWin::Companion panel;panel.setSession(&s);
        QImage image(40,30,QImage::Format_RGB32);image.fill(Qt::blue);QByteArray jpeg;QBuffer buffer(&jpeg);buffer.open(QIODevice::WriteOnly);QVERIFY(image.save(&buffer,"JPEG"));
        Message frame{Frame,99,10,{}};put(frame.data,1);put(frame.data,40);put(frame.data,30);frame.data.insert(frame.data.end(),jpeg.begin(),jpeg.end());auto encoded=encode(frame);
        auto deliver=[&]{emit s.transient(QByteArray(reinterpret_cast<const char*>(encoded.data()),qsizetype(encoded.size())));QCoreApplication::processEvents();};
        deliver();QWidget *surface=nullptr;for(auto widget:QApplication::topLevelWidgets())if(widget->windowTitle().startsWith("Synthetic host ·"))surface=widget;
        QVERIFY(surface);QVERIFY(surface->isVisible());surface->activateWindow();QCoreApplication::processEvents();
        if(qEnvironmentVariableIsSet("WAYWIN_TEST_ARTIFACTS"))surface->grab().save(qEnvironmentVariable("WAYWIN_TEST_ARTIFACTS")+"/shell-synthetic.png");
        surface->showMinimized();QCoreApplication::processEvents();surface->showNormal();QCoreApplication::processEvents();
        surface->hide();QCoreApplication::processEvents();deliver();QVERIFY(!surface->isVisible()); // A late frame cannot undo Hide.
        surface->close();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QByteArray output;char bytes[4096];ssize_t count=0;while((count=::read(pipe[0],bytes,sizeof(bytes)))>0)output.append(bytes,count);
        Decoder decoder;auto commands=decoder.feed(reinterpret_cast<const uint8_t*>(output.constData()),size_t(output.size()));bool hidden=false,visible=false,active=false;
        for(const auto&m:commands)if(m.type==SurfaceState){Reader read(m.data);QCOMPARE(read.number(),99u);auto state=read.number();QVERIFY(state<=Active);hidden|=state==Hidden;visible|=state!=Hidden;active|=state==Active;}
        QVERIFY(hidden);QVERIFY(visible);QVERIFY(active);
        delete s.proxyWriter;s.proxyWriter=nullptr;::close(pipe[0]);::close(pipe[1]);
    }
    void unresponsiveCompanionStopsOnlyThisHost_data(){QTest::addColumn<bool>("freezeWorker");QTest::newRow("unresponsive-companion")<<false;QTest::newRow("stopped-session-worker")<<true;}
    void unresponsiveCompanionStopsOnlyThisHost(){
        using namespace WayWinWire;
        QFETCH(bool,freezeWorker);
        QTemporaryDir temp;WayWin::Host host;WayWin::Session s(host,temp.path());QProcess other;
        const auto program=QStringLiteral(WAYWIN_EXECUTABLE);const QStringList arguments={"--session-worker","/bin/sh","-c","read credential; trap '' TERM; sleep 60"};
        s.worker.start(program,arguments);QVERIFY(s.worker.waitForStarted());s.commandWriter.enqueue({"synthetic\n",true});
        other.start(program,arguments);QVERIFY(other.waitForStarted());other.write("synthetic\n");
        QTRY_VERIFY_WITH_TIMEOUT(s.pid>0,3000);
        if(freezeWorker)QCOMPARE(::kill(s.worker.processId(),SIGSTOP),0);
        s.state="연결됨";s.companionReady=s.shellReady=true;
        std::vector<uint32_t> ids;
        s.windows.insert(77);
        for(unsigned i=0;i<64;++i){if(i%2){s.command(1,77);QVERIFY(s.inputRequests.contains(s.nextRequest));}else{std::vector<uint8_t>d;put(d,1);put(d,16);put(d,1);auto id=s.request(Key,d);QVERIFY(id);ids.push_back(id);}}
        QCOMPARE(s.requests.size(),64);QVERIFY(s.inputBlocked);QCOMPARE(s.request(Text,{}),0u);
        std::vector<uint8_t>release;put(release,1);put(release,16);put(release,0);QVERIFY(s.request(Key,release));QCOMPARE(s.controlRequests.size(),1);
        QElapsedTimer elapsed;elapsed.start();QTRY_VERIFY_WITH_TIMEOUT(s.failed,6500);QVERIFY(elapsed.elapsed()>=4500);
        QTRY_COMPARE_WITH_TIMEOUT(s.worker.state(),QProcess::NotRunning,4500);QVERIFY(elapsed.elapsed()<10000);QCOMPARE(s.crashRestarts,0u);QCOMPARE(other.state(),QProcess::Running);
        other.closeWriteChannel();QVERIFY(other.waitForFinished(4500));
    }
    void unreadEngineHasBoundedTransport(){
        using namespace WayWinWire;
        QProcess worker,other;const auto program=QStringLiteral(WAYWIN_EXECUTABLE);
        const QStringList args={"--session-worker","/bin/sh","-c","read credential; trap '' TERM; sleep 60"};
        worker.start(program,args);QVERIFY(worker.waitForStarted());worker.write("synthetic\n");
        other.start(program,args);QVERIFY(other.waitForStarted());other.write("synthetic\n");QTest::qWait(100);
        WayWin::CommandWriter writer(&worker);bool failed=false;writer.fault=[&]{failed=true;worker.closeWriteChannel();};
        Message text{Text,1,1,{}};put(text.data,1);string(text.data,std::string(4096,'x'));auto bytes=encode(text);
        auto command="20 0 0 1 "+hex(bytes);size_t maximum=0;
        for(int n=0;n<100;++n){writer.enqueue(WayWinPolicy::commandPacket(command));maximum=std::max(maximum,writer.bytes());}
        QVERIFY(maximum<=WayWinPolicy::NormalBytes);QVERIFY(worker.bytesToWrite()<=qint64(command.size()+1));
        QTRY_VERIFY_WITH_TIMEOUT(failed||worker.state()==QProcess::NotRunning,10000);
        QTRY_COMPARE_WITH_TIMEOUT(worker.state(),QProcess::NotRunning,4500);QCOMPARE(other.state(),QProcess::Running);
        other.closeWriteChannel();QVERIFY(other.waitForFinished(4500));
    }

    void rejectUntrustedManifest(){QTemporaryDir temp;auto f=temp.filePath("waywin-runtime.json");QVERIFY(WayWin::saveJson(f,{{"schema",2}},nullptr));QString error;QVERIFY(WayWin::verifyRuntime(f,&error).isEmpty());QVERIFY(!error.isEmpty());}
};
QTEST_MAIN(ProductTests)
#include "test_product.moc"
