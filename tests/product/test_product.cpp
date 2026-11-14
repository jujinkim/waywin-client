// SPDX-License-Identifier: Apache-2.0
#include "core.h"
#include "window.h"
#include "validation.h"
#include <QListWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QDialog>
#include <QTimer>
#include <QTabWidget>
#include <csignal>
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QFile>
#include <QUuid>
#include <QProcess>
#include <QSignalSpy>
#include <unistd.h>
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
        qputenv("FREERDP_PASSWORD","secret");qputenv("https_proxy","secret");qputenv("WLOG_FILEAPPENDER_OUTPUT_FILE_PATH","secret");qputenv("SDL_VIDEODRIVER","x11");
        auto env=WayWin::sessionEnvironment("/private/test");
        QVERIFY(!env.contains("FREERDP_PASSWORD"));QVERIFY(!env.contains("https_proxy"));QVERIFY(!env.contains("WLOG_FILEAPPENDER_OUTPUT_FILE_PATH"));QCOMPARE(env.value("SDL_VIDEODRIVER"),QString("wayland"));
        qunsetenv("FREERDP_PASSWORD");qunsetenv("https_proxy");qunsetenv("WLOG_FILEAPPENDER_OUTPUT_FILE_PATH");qunsetenv("SDL_VIDEODRIVER");
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
        auto r=WayWin::newReport();QCOMPARE(r["schema"].toInt(),2);QCOMPARE(r["cases"].toObject().size(),10);QCOMPARE(r["monitor_matrix"].toObject().size(),6);
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
        auto cases=r["cases"].toObject();cases.remove("P09");r["cases"]=cases;QVERIFY(!WayWin::assessReport(r).isEmpty());
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
                dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
            });add->click();
            QCOMPARE(window.findChild<QListWidget*>("hostList")->count(),1);
            QVERIFY(QFileInfo::exists(temp.filePath("hosts.json")));
            auto v=window.findChild<WayWin::Validation*>();QVERIFY(v);QVERIFY(v->isEnabled());
            v->attachRun({{"run_id","test-run"},{"sdl_driver_observed","wayland"}});
            QCOMPARE(v->report()["environment"].toObject()["sdl_driver"].toString(),QString("wayland"));
        }
        WayWin::Window reopened(temp.path());
        if(qEnvironmentVariableIsSet("WAYWIN_TEST_ARTIFACTS")){reopened.show();auto tabs=reopened.findChild<QTabWidget*>();for(int i=0;i<tabs->count();++i){tabs->setCurrentIndex(i);QCoreApplication::processEvents();reopened.grab().save(qEnvironmentVariable("WAYWIN_TEST_ARTIFACTS")+QString("/tab-%1.png").arg(i));}}
        QCOMPARE(reopened.findChild<QListWidget*>("hostList")->count(),1);
        auto v=reopened.findChild<WayWin::Validation*>();QCOMPARE(v->report()["environment"].toObject()["sdl_driver"].toString(),QString("wayland"));
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
        QVERIFY(file.open(QIODevice::WriteOnly));file.write("changed");file.close();QVERIFY(WayWin::verifyRuntime(manifest,&error).isEmpty());
        QVERIFY(file.open(QIODevice::WriteOnly));file.write("fixture");file.close();
        QFile extra(temp.filePath("libextra.so"));QVERIFY(extra.open(QIODevice::WriteOnly));extra.write("unlisted");extra.close();QVERIFY(WayWin::verifyRuntime(manifest,&error).isEmpty());extra.remove();
        QTemporaryDir outside;QFile external(outside.filePath("engine"));QVERIFY(external.open(QIODevice::WriteOnly));external.write("fixture");external.close();QFile::remove(binary);QVERIFY(QFile::link(external.fileName(),binary));QVERIFY(WayWin::verifyRuntime(manifest,&error).isEmpty());
    }
    void rejectUntrustedManifest(){QTemporaryDir temp;auto f=temp.filePath("waywin-runtime.json");QVERIFY(WayWin::saveJson(f,{{"schema",2}},nullptr));QString error;QVERIFY(WayWin::verifyRuntime(f,&error).isEmpty());QVERIFY(!error.isEmpty());}
};
QTEST_MAIN(ProductTests)
#include "test_product.moc"
