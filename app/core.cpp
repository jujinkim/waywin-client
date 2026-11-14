// SPDX-License-Identifier: Apache-2.0
#include "core.h"
#include "../protocol/waywin_wire.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>
#include <QUrl>
#include <QUuid>
#include <sys/stat.h>
namespace WayWin {
static bool textValid(const QString &s, bool optional = false) {
    if (s.isEmpty()) return optional;
    if (s.size() > 256) return false;
    for (const auto c : s) if (c.unicode() < 32 || c.unicode() == 127) return false;
    return true;
}
QString endpoint(const QString &input, QString *error) {
    auto fail = [&]() { if (error) *error = "IP·DNS 주소와 포트만 입력하세요. 계정·경로·쿼리는 주소에 포함할 수 없습니다."; return QString(); };
    if (!textValid(input) || input.contains(QRegularExpression("\\s"))) return fail();
    QString value = input;
    if (!value.contains("://") && value.count(':') > 1 && !value.startsWith('[')) {
        QHostAddress ip(value);
        if (ip.protocol() != QAbstractSocket::IPv6Protocol) return fail();
        value = '[' + value + ']';
    }
    QUrl url(value.contains("://") ? value : "rdp://" + value, QUrl::StrictMode);
    const int port = url.port(3389);
    if (!url.isValid() || url.scheme() != "rdp" || !url.userInfo().isEmpty() ||
        url.authority().contains('@') || !url.path().isEmpty() || url.hasQuery() || url.hasFragment() ||
        port < 1 || port > 65535 || value.endsWith(':')) return fail();
    QString host = url.host();
    QHostAddress ip(host);
    if (ip.protocol() == QAbstractSocket::IPv6Protocol) {
        if (!ip.scopeId().isEmpty() && !QRegularExpression("^[A-Za-z0-9._-]{1,15}$").match(ip.scopeId()).hasMatch()) return fail();
        host = '[' + ip.toString() + ']';
    } else if (ip.protocol() == QAbstractSocket::IPv4Protocol) {
        // Only canonical dotted decimal, not resolver-specific octal/short forms.
        QString rawHost = value;
        if (rawHost.startsWith("rdp://")) rawHost.remove(0,6);
        rawHost = rawHost.section(':',0,0);
        if (rawHost != ip.toString()) return fail();
        host = ip.toString();
    } else {
        host = QString::fromLatin1(QUrl::toAce(host));
        if (host.isEmpty() || host.size() > 253) return fail();
        QString dns = host;
        if (dns.endsWith('.')) dns.chop(1);
        const auto parts = dns.split('.');
        for (const auto &part : parts)
            if (!QRegularExpression("^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$").match(part).hasMatch()) return fail();
        if (QRegularExpression("^(?:[0-9]+|0[xX][0-9A-Fa-f]*)$").match(parts.last()).hasMatch()) return fail();
    }
    return host + ':' + QString::number(port);
}
QJsonObject Host::json() const {
    return {{"id",id},{"name",name},{"server",server},{"user",user},{"domain",domain},{"app",app},
        {"mode",mode},{"monitors",monitors},{"width",width},{"height",height},{"depth",depth},
        {"multimon",multimon},{"dynamic",dynamic},{"sound",sound},{"remember",remember}};
}
Host Host::fromJson(const QJsonObject &o) {
    Host h;
    h.id=o["id"].toString(); h.name=o["name"].toString(); h.server=o["server"].toString();
    h.user=o["user"].toString(); h.domain=o["domain"].toString(); h.app=o["app"].toString();
    h.mode=o["mode"].toString("desktop"); h.monitors=o["monitors"].toString();
    h.width=o["width"].toInt(1280); h.height=o["height"].toInt(720); h.depth=o["depth"].toInt(32);
    h.multimon=o["multimon"].toBool(); h.dynamic=o["dynamic"].toBool();
    h.sound=o["sound"].toBool(); h.remember=o["remember"].toBool(); return h;
}
QString Host::validate() const {
    if (QUuid(id).isNull()) return "호스트 ID가 올바르지 않습니다.";
    if (!textValid(name) || !textValid(user) || user.contains('\\') || !textValid(domain,true)) return "이름·사용자명을 입력하고 도메인은 별도 칸에 입력하세요.";
    QString error; endpoint(server,&error); if (!error.isEmpty()) return error;
    if (mode!="desktop" && mode!="rail") return "연결 모드가 올바르지 않습니다.";
    if (!textValid(app,true) || app.contains(',')) return "앱 별칭·경로에는 쉼표와 제어 문자를 사용할 수 없습니다.";
    if (width<200 || width>8192 || height<200 || height>8192 || !QList<int>{16,24,32}.contains(depth)) return "화면 크기·색 깊이가 올바르지 않습니다.";
    if (!monitors.isEmpty()) {
        if ((!multimon && mode!="rail") || !QRegularExpression("^[0-9]{1,5}(,[0-9]{1,5}){0,15}$").match(monitors).hasMatch()) return "화면 ID는 다중 화면 또는 개별 창 모드에서 쉼표로 구분해 입력하세요.";
        QSet<int> ids;
        for (const auto &part : monitors.split(',')) {
            const int id=part.toInt(); if (id<1 || id>65535 || ids.contains(id)) return "화면 ID는 중복 없이 1–65535 사이여야 합니다.";
            ids.insert(id);
        }
    }
    return {};
}
QStringList arguments(const Host &h, bool launch) {
    QStringList a{"/v:"+endpoint(h.server),"/u:"+h.user,"/t:["+h.name+"] WayWin",
        "/from-stdin:force","/sec:rdp:off","/proxy:ignore","/log-level:ERROR","+auto-reconnect","/dvc:waywin",
        "-clipboard","/timeout:9000","/bpp:"+QString::number(h.depth)};
    if (!h.domain.isEmpty()) a << "/d:"+h.domain;
    a << (h.sound ? "/sound" : "/audio-mode:2");
    if (h.multimon || h.mode=="rail") {
        a << "/multimon";
        if (!h.monitors.isEmpty()) a << "/monitors:"+h.monitors;
    } else a << "-multimon" << QString("/size:%1x%2").arg(h.width).arg(h.height);
    a << (h.dynamic ? "+dynamic-resolution" : "-dynamic-resolution");
    if (h.mode=="rail") a << "/app:program:"+(launch ? h.app : QString());
    return a;
}
QString Host::accent() const {
    const auto hash=QCryptographicHash::hash(id.toUtf8(),QCryptographicHash::Sha256);
    return QStringList{"#7651c9","#187a9d","#bb5a24","#30804b","#b34472","#6166bc"}.at(uchar(hash[0])%6);
}
QProcessEnvironment sessionEnvironment(const QString &certDir) {
    auto env=QProcessEnvironment::systemEnvironment();
    for (const auto &key : env.keys()) {
        const auto upper=key.toUpper();
        if (upper.startsWith("FREERDP_") || upper.startsWith("WLOG_") || upper.startsWith("SDL_") ||
            upper.startsWith("LD_") || upper.startsWith("DYLD_") || upper.startsWith("OPENSSL_") ||
            upper.startsWith("SSL_CERT_") || upper.endsWith("_PROXY") || upper=="ALL_PROXY") env.remove(key);
    }
    const auto ime=qgetenv("SDL_IM_MODULE");
    if (ime=="ibus" || ime=="fcitx") env.insert("SDL_IM_MODULE",QString::fromLatin1(ime));
    env.insert("WAYWIN_PROBE_EVENTS","1"); env.insert("WAYWIN_PRODUCT","1");
    env.insert("WAYWIN_CERT_DIR",certDir); env.insert("SDL_VIDEODRIVER","wayland");
    return env;
}
QString privateDirectory(const QString &path) {
    if (!QDir().mkpath(path) || !QFile::setPermissions(path,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner)) return {};
    return path;
}
bool saveJson(const QString &path,const QJsonObject &o,QString *error) {
    QSaveFile f(path);
    const auto bytes=QJsonDocument(o).toJson();
    if (!f.open(QIODevice::WriteOnly) || !f.setPermissions(QFile::ReadOwner|QFile::WriteOwner) ||
        f.write(bytes)!=bytes.size() || !f.commit()) {
        if (error) *error="파일을 안전하게 저장하지 못했습니다. 폴더 권한과 여유 공간을 확인하세요.";
        return false;
    }
    return true;
}
QJsonObject readJson(const QString &path,QString *error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size()>8*1024*1024) { if(error)*error="파일을 읽을 수 없거나 크기가 너무 큽니다."; return {}; }
    QJsonParseError parse;
    auto doc=QJsonDocument::fromJson(f.readAll(),&parse);
    if (parse.error!=QJsonParseError::NoError || !doc.isObject()) {if(error)*error="JSON 파일 형식이 올바르지 않습니다."; return {};}
    return doc.object();
}
QJsonObject environment() {
    const auto env=QProcessEnvironment::systemEnvironment();
    QString display=env.value("WAYLAND_DISPLAY");
    if (!display.startsWith('/')) display=env.value("XDG_RUNTIME_DIR")+'/'+display;
    struct stat st{};
    const bool socket=!env.value("WAYLAND_DISPLAY").isEmpty() && ::stat(display.toLocal8Bit().constData(),&st)==0 && S_ISSOCK(st.st_mode);
    QJsonObject checks{{"linux",QSysInfo::kernelType()=="linux"},{"ubuntu_26_04",QSysInfo::productType()=="ubuntu" && QSysInfo::productVersion()=="26.04"},
        {"gnome",env.value("XDG_CURRENT_DESKTOP").contains("gnome",Qt::CaseInsensitive)},
        {"wayland_session",env.value("XDG_SESSION_TYPE")=="wayland"},{"wayland_socket",socket}};
    bool ready=true; for(auto it=checks.begin();it!=checks.end();++it) ready &= it.value().toBool();
    return {{"system",QSysInfo::prettyProductName()},{"machine",QSysInfo::currentCpuArchitecture()},
        {"checks",checks},{"target_ready",ready},{"runtime_verified",false}};
}
QString verifyRuntime(const QString &manifest,QString *error,QJsonObject *metadata) {
    const auto m=readJson(manifest,error);
    auto fail=[&](const QString &s){if(error)*error=s;return QString();};
    if (m["schema"].toString()!="waywin-runtime-1" || m["source_commit"].toString()!=WAYWIN_FREERDP_COMMIT ||
        m["product_patch_sha256"].toString()!=WAYWIN_PRODUCT_PATCH || m["probe_patch_sha256"].toString()!=WAYWIN_PROBE_PATCH)
        return fail("이 제품 버전에 맞는 세션 엔진이 아닙니다. WayWin 런타임을 다시 빌드하거나 설치하세요.");
    const QDir root(QFileInfo(manifest).absolutePath());
    const QString base=QFileInfo(root.path()).canonicalFilePath()+'/';
    const auto files=m["files"].toObject();
    if (files.isEmpty() || files.size()>10000 || !files.contains(m["binary"].toString())) return fail("엔진 파일 목록이 올바르지 않습니다.");
    for(auto it=files.begin();it!=files.end();++it) {
        QFileInfo fi(root.filePath(it.key()));
        if (QDir::isAbsolutePath(it.key()) || it.key().split('/').contains("..") || !fi.canonicalFilePath().startsWith(base)) return fail("엔진 파일 경로가 안전하지 않습니다.");
        QFile f(fi.filePath());
        if (!f.open(QIODevice::ReadOnly)) return fail("엔진 파일을 읽을 수 없습니다.");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&f) || QString::fromLatin1(hash.result().toHex())!=it.value().toString()) return fail("엔진 파일이 변경되었거나 손상되었습니다. 다시 설치하세요.");
    }
    QDirIterator scan(root.path(),QDir::Files,QDirIterator::Subdirectories);
    while(scan.hasNext()) {
        const auto path=scan.next(); const auto name=scan.fileName();
        if ((name.contains(".so") || name.contains(".dylib")) && !files.contains(root.relativeFilePath(path))) return fail("명세에 없는 라이브러리가 발견되었습니다.");
    }
    if (metadata) *metadata=m;
    const auto binary=root.filePath(m["binary"].toString());
    if (!QFileInfo(binary).isExecutable()) return fail("세션 엔진 실행 권한이 없습니다.");
    return binary;
}
QJsonObject parseEvent(const QByteArray &line) {
    if(line.startsWith("WAYWIN_UI_V2 ") && line.endsWith('\n')) {
        try {
            auto hex=line.mid(13,line.size()-14);auto bytes=WayWinWire::unhex(hex.toStdString());
            WayWinWire::Decoder decoder;auto messages=decoder.feed(bytes.data(),bytes.size());
            if(messages.size()!=1 || WayWinWire::encode(messages[0])!=bytes || !WayWinWire::responseType(messages[0].type))return {};
            return {{"type","ui"},{"payload",QString::fromLatin1(hex)}};
        }catch(...){return {};}
    }
    const QString s=QString::fromLatin1(line);
    auto m=QRegularExpression("^WAYWIN_PROBE_V1 driver=(wayland|x11|cocoa|windows)\\n$").match(s);
    if(m.hasMatch())return {{"type","driver"},{"driver",m.captured(1)}};
    m=QRegularExpression("^WAYWIN_PRODUCT_V1 (connected|disconnected) code=([0-9]{1,10})\\n$").match(s);
    if(m.hasMatch())return {{"type",m.captured(1)},{"code",double(m.captured(2).toULongLong())}};
    m=QRegularExpression("^WAYWIN_PROBE_V1 type=(window-create|window-delete|notify-create|notify-update|notify-delete|desktop-monitored|desktop-unmonitored) window=([0-9]{1,10}) icon=([0-9]{1,10}) flags=([0-9]{1,10})\\n$").match(s);
    if(!m.hasMatch())return {};
    QJsonObject o{{"type",m.captured(1)}};
    for(int i=2;i<=4;++i) {auto n=m.captured(i).toULongLong();if(n>0xffffffffULL)return {};o[QStringList{"window","icon","flags"}[i-2]]=double(n);}
    return o;
}
QList<QJsonObject> EventDecoder::feed(const QByteArray &chunk) {
    QList<QJsonObject> result;
    for(char c:chunk) {
        if(c=='\n') {
            if(!dropping) {auto event=parseEvent(pending+'\n');if(!event.isEmpty())result.append(event);}
            pending.clear();dropping=false;
        } else if(!dropping) {
            pending+=c;
            const auto limit=pending.startsWith("WAYWIN_UI_V2 ")?2*(WayWinWire::Maximum+24)+16:512;
            if(pending.size()>limit){pending.clear();dropping=true;}
        }
    }
    return result;
}
QJsonObject newReport() {
    auto r=readJson(":/validation/feasibility-report.example.json");
    r["date"]=QDate::currentDate().toString(Qt::ISODate);r["independently_verified"]=false;
    return r;
}
QStringList assessReport(const QJsonObject &r) {
    QStringList errors;
    if(r["schema"].toInt()!=2)errors<<"보고서 버전이 올바르지 않습니다.";
    if(r["tester"].toString().trimmed().isEmpty())errors<<"검증자 이름을 입력하세요.";
    if(!QDate::fromString(r["date"].toString(),Qt::ISODate).isValid())errors<<"검증 날짜를 입력하세요.";
    const auto env=r["environment"].toObject();
    for(const auto &key:newReport()["environment"].toObject().keys())if(env[key].toString().trimmed().isEmpty())errors<<"환경 정보 누락: "+key;
    if(env["sdl_driver"].toString()!="wayland")errors<<"실제 Wayland 드라이버 관측이 필요합니다.";
    const auto apps=r["applications"].toArray();if(apps.size()<5)errors<<"대표 앱 5개 이상의 이름과 버전을 기록하세요.";
    for(const auto &v:apps){const auto a=v.toObject();if(a["name"].toString().isEmpty()||a["version"].toString().isEmpty())errors<<"앱 이름·버전이 누락되었습니다.";}
    const auto base=newReport();
    for(const auto &group:QStringList{"cases","shell_surfaces","monitor_matrix"}) {
        const auto items=r[group].toObject();
        for(const auto &key:base[group].toObject().keys()) {
            const auto item=items[key].toObject();
            if(group=="cases" && key=="P08")continue;
            if(item["status"].toString()!="pass" || item["notes"].toString().trimmed().isEmpty() || item["evidence"].toArray().isEmpty())errors<<key+": 합격 관측과 증거가 필요합니다.";
            for(const auto &v:item["evidence"].toArray())if(v.toString().trimmed().isEmpty())errors<<key+": 빈 증거 참조입니다.";
            if(group=="shell_surfaces")for(const auto &f:QStringList{"remote_ui_visible","interactive","same_session"})if(!item[f].toBool())errors<<key+": "+f+" 확인이 필요합니다.";
            if(group=="monitor_matrix")for(const auto &f:QStringList{"host_physical_layout","client_layout","rdp_session_layout"})if(item[f].toString().trimmed().isEmpty())errors<<key+": "+f+"를 기록하세요.";
        }
    }
    return errors;
}
QString caseInstructions(const QString &id) {
    static const QMap<QString,QString> text{
        {"P01","개별 창을 이동하고 크기를 5회 변경하세요. 한영 입력·조합키·팝업을 100/150/200%에서 확인하세요. 좌표·포커스 오류가 생기면 기록 후 종료합니다."},
        {"P02","전체 데스크톱에서 시험 앱과 대표 앱 5개를 열고 접속을 해제하세요. 앱 실행 없이 개별 창 모드로 다시 연결해 기존 창과 새 창을 확인하세요."},
        {"P03","창을 가리고 최소화·복원·최대화를 반복하세요. 임시 입력과 경과 시간이 유지되는지 확인합니다."},
        {"P04","Windows 시험 앱의 Tray 메뉴로 아이콘 생성·변경·삭제를 3회 반복하세요. Ubuntu에서 아이콘과 좌클릭·우클릭 메뉴가 실제로 작동해야 합격입니다. 이벤트 수신만으로 합격하지 않습니다."},
        {"P05","전체 데스크톱과 개별 창을 전환하세요. Windows Session/PID/Instance 및 임시 입력을 비교하고 앱이 추가 실행되거나 작업이 사라지면 기록하세요."},
        {"P06","네트워크를 끊었다 복원한 뒤 다시 연결하세요. 앱 재실행 없이 20회 반복하고 세션 보존·중복 실행·복구 결과를 기록하세요."},
        {"P07","두 호스트에 연결하세요. 선택 호스트 일시 정지·재개·강제 종료 중 다른 호스트와 관리 화면의 입력 반응을 확인하세요."},
        {"P08","전체 데스크톱과 선택 창을 동시에 표시할 수 있는지 조사하세요. 선택 항목입니다."},
        {"P09","Windows shell 메뉴의 시작 메뉴·IME 설정·빠른 설정을 각각 여세요. 원본 UI가 Ubuntu에 별도 표시되어 입력·닫기·포커스 복귀가 되는지 확인하세요. 아래 셸별 결과도 채우세요."},
        {"P10","물리 호스트 화면과 Ubuntu 화면을 구분해 M01–M06을 기록하세요. 음수 좌표·회전·혼합 DPI·hotplug·화면 복구를 포함합니다."}};
    return text.value(id,"해당 구성에서 실제 표시·입력·동일 세션을 확인하고 관측과 증거를 기록하세요. 실패하면 반복 조작을 중지합니다.");
}
}
