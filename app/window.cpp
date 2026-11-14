// SPDX-License-Identifier: Apache-2.0
#include "window.h"
#include "companion.h"
#include <QImage>
#include "validation.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <qt6keychain/keychain.h>
namespace WayWin {
static void deleteSecret(QObject *parent,const QString &id,std::function<void(bool)> done) {
    auto job=new QKeychain::DeletePasswordJob("WayWin",parent);job->setKey(id);job->setInsecureFallback(false);
    QObject::connect(job,&QKeychain::Job::finished,parent,[job,done]{done(job->error()==QKeychain::NoError || job->error()==QKeychain::EntryNotFound);});job->start();
}
Window::Window(const QString &dataRoot,QWidget *parent):QMainWindow(parent) {
    setWindowIcon(QIcon(":/icons/waywin-256.png"));setWindowTitle("WayWin");resize(1120,820);setMinimumSize(820,640);
    root=privateDirectory(dataRoot.isEmpty()?QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation):dataRoot);
    if(root.isEmpty()) {configWritable=false;QTimer::singleShot(0,this,[this]{showError("제품 데이터 폴더를 만들 수 없습니다.");});}
    runtime=WAYWIN_DEFAULT_RUNTIME;
    const auto installed=QCoreApplication::applicationDirPath()+"/../lib/waywin/runtime/waywin-runtime.json";
    if(QFileInfo::exists(installed))runtime=QFileInfo(installed).absoluteFilePath();
    if(!root.isEmpty() && QFileInfo::exists(root+"/hosts.json")) {
        QString error;auto config=readJson(root+"/hosts.json",&error);
        if(!error.isEmpty() || config["schema"].toInt()!=1) {configWritable=false;QTimer::singleShot(0,this,[this]{showError("기존 호스트 설정을 읽지 못했습니다. 원본을 보존했습니다. 데이터 폴더에서 파일을 복구한 뒤 다시 시작하세요.");});}
        else {
            runtime=config["runtime"].toString(runtime);QSet<QString> ids;
            for(const auto &v:config["hosts"].toArray()) {
                auto h=Host::fromJson(v.toObject());
                if(!h.validate().isEmpty() || ids.contains(h.id)){configWritable=false;break;}
                ids.insert(h.id);hosts.append(h);
            }
            if(!configWritable)QTimer::singleShot(0,this,[this]{showError("저장된 호스트에 잘못된 값 또는 중복 ID가 있습니다. 원본 설정을 복구하세요.");});
        }
    }
    auto split=new QSplitter;setCentralWidget(split);
    auto sidebar=new QWidget;auto side=new QVBoxLayout(sidebar);split->addWidget(sidebar);
    auto brand=new QLabel("WayWin");auto font=brand->font();font.setPointSize(22);font.setBold(true);brand->setFont(font);side->addWidget(brand);
    side->addWidget(new QLabel("Windows 작업 공간"));list=new QListWidget;list->setObjectName("hostList");list->setMinimumWidth(210);side->addWidget(list);
    auto add=new QPushButton("호스트 추가");add->setObjectName("addHost");side->addWidget(add);
    editButton=new QPushButton("설정 편집");deleteButton=new QPushButton("호스트 삭제");side->addWidget(editButton);side->addWidget(deleteButton);
    auto content=new QWidget;auto layout=new QVBoxLayout(content);split->addWidget(content);split->setStretchFactor(1,1);
    title=new QLabel("호스트를 추가하세요");title->setFont(font);title->setTextFormat(Qt::PlainText);layout->addWidget(title);
    state=new QLabel;detail=new QLabel;detail->setWordWrap(true);layout->addWidget(state);layout->addWidget(detail);
    auto connection=new QHBoxLayout;layout->addLayout(connection);
    mode=new QComboBox;mode->addItem("전체 데스크톱","desktop");mode->addItem("개별 창 (RemoteApp)","rail");connection->addWidget(mode);
    launch=new QCheckBox("등록 앱 새로 실행");launch->setToolTip("재접속과 모드 전환에서는 새 프로그램을 실행하지 않습니다.");connection->addWidget(launch);
    connectButton=new QPushButton("연결");connectButton->setObjectName("connectHost");disconnectButton=new QPushButton("연결 해제");connection->addWidget(connectButton);connection->addWidget(disconnectButton);
    auto reconnect=new QPushButton("다시 연결");auto switchButton=new QPushButton("모드 전환");connection->addWidget(reconnect);connection->addWidget(switchButton);
    tabs=new QTabWidget;layout->addWidget(tabs);
    auto overview=new QWidget;auto overviewLayout=new QVBoxLayout(overview);
    auto description=new QLabel("인증서는 접속 창에서 이름과 지문을 확인하세요. 여러 호스트를 동시에 연결할 수 있습니다.\n개별 창은 Windows의 RemoteApp 허용 설정이 필요합니다.");description->setWordWrap(true);overviewLayout->addWidget(description);
    windows=new QListWidget;overviewLayout->addWidget(new QLabel("수신된 원격 창·알림 아이콘"));overviewLayout->addWidget(windows);
    auto capability=new QLabel("원격 창·알림 아이콘을 선택해 아래 버튼으로 조작하세요. 창 제목과 원본 알림 아이콘은 이 호스트에서 수신한 정보입니다.");capability->setWordWrap(true);overviewLayout->addWidget(capability);
    auto remoteActions=new QHBoxLayout;overviewLayout->addLayout(remoteActions);
    int actionId=0;
    for(const auto &label:QStringList{"창 활성화","최소화","복원","창 닫기","아이콘 클릭","아이콘 메뉴"}) {
        auto button=new QPushButton(label);const auto id=++actionId;remoteActions->addWidget(button);
        connect(button,&QPushButton::clicked,this,[this,id]{auto h=selected();auto item=windows->currentItem();if(!h||!item)return;auto session=sessions.value(h->id);if(!session)return;
            if(id==4&&QMessageBox::question(this,"원격 창 닫기","Windows 앱에 창 닫기를 요청할까요? 저장하지 않은 작업을 확인하세요.")!=QMessageBox::Yes)return;
            const auto target=item->data(Qt::UserRole).toULongLong();const bool tray=item->data(Qt::UserRole+1).toBool();
            if((id>=5)!=tray)return;
            session->command(id,tray?quint32(target>>32):quint32(target),tray?quint32(target):0);
        });
    }
    auto latest=new QPushButton("현재 실행을 선택 검증 항목에 첨부");overviewLayout->addWidget(latest);
    tabs->addTab(overview,"연결");
    validation=new Validation;tabs->addTab(validation,"실환경 검증");
    auto setup=new QWidget;auto setupLayout=new QVBoxLayout(setup);
    environmentLabel=new QLabel;environmentLabel->setWordWrap(true);environmentLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);setupLayout->addWidget(environmentLabel);
    auto check=new QPushButton("환경 다시 점검");setupLayout->addWidget(check);
    runtimeLabel=new QLabel;runtimeLabel->setWordWrap(true);runtimeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);setupLayout->addWidget(runtimeLabel);
    auto selectRuntime=new QPushButton("세션 엔진 선택…");setupLayout->addWidget(selectRuntime);
    auto monitors=new QPushButton("SDL 화면 ID 조회");setupLayout->addWidget(monitors);
    auto folder=new QPushButton("기록 폴더 열기");setupLayout->addWidget(folder);
    auto help=new QLabel("검증 순서: 환경 점검 → 호스트 추가 → 전체 데스크톱 연결 → Windows 시험 앱 실행 → 개별 창 재접속 → 검증 항목 기록 → 보고서 내보내기.\nWindows 시험 앱의 Documents / Tray / Windows shell 메뉴에서 시험 동작을 실행합니다.");help->setWordWrap(true);setupLayout->addWidget(help);setupLayout->addStretch();tabs->addTab(setup,"환경·도움말");
    companion=new Companion;tabs->addTab(companion,"Windows 앱·셸·클립보드");
    auto recoveryBar=new QHBoxLayout;overviewLayout->addLayout(recoveryBar);
    for(const auto &entry:QList<QPair<QString,unsigned>>{{"즉시 재시도",21},{"자동 재시도 중단",22}}){auto button=new QPushButton(entry.first);recoveryBar->addWidget(button);connect(button,&QPushButton::clicked,this,[this,entry]{auto h=selected();if(h&&sessions.contains(h->id))sessions[h->id]->recovery(entry.second);});}
    auto test=menuBar()->addMenu("검증 도구");
    for(const auto &pair:QList<QPair<QString,QString>>{{"선택 세션 일시 정지","pause"},{"선택 세션 재개","resume"},{"선택 세션 강제 종료","crash"}}) {
        auto action=test->addAction(pair.first);connect(action,&QAction::triggered,this,[this,pair]{auto h=selected();if(!h)return;auto s=sessions.value(h->id);if(!s||!s->active())return;if(pair.second=="crash" && QMessageBox::question(this,"장애 시험",h->name+"의 로컬 세션 프로세스를 강제 종료할까요?")!=QMessageBox::Yes)return;s->inject(pair.second);});
    }
    auto forget=test->addAction("선택 호스트의 저장 비밀번호 삭제");connect(forget,&QAction::triggered,this,[this]{auto h=selected();if(!h)return;deleteSecret(this,h->id,[this](bool ok){if(!ok)showError("키링 삭제에 실패했습니다. 키링 잠금을 해제하고 다시 시도하세요.");else statusBar()->showMessage("저장 비밀번호를 삭제했습니다.",5000);});});
    connect(add,&QPushButton::clicked,this,[this]{editHost(true);});connect(editButton,&QPushButton::clicked,this,[this]{editHost(false);});connect(deleteButton,&QPushButton::clicked,this,&Window::removeHost);
    connect(list,&QListWidget::currentRowChanged,this,[this]{auto h=selected();if(h){mode->setCurrentIndex(mode->findData(h->mode));launch->setChecked(false);validation->setHost(root+"/validation-"+h->id+".json");}refresh();});
    connect(mode,&QComboBox::currentIndexChanged,this,[this]{launch->setEnabled(mode->currentData()=="rail");});
    connect(connectButton,&QPushButton::clicked,this,[this]{connectHost();});connect(disconnectButton,&QPushButton::clicked,this,[this]{auto h=selected();if(h&&sessions.contains(h->id))sessions[h->id]->stop();});
    connect(reconnect,&QPushButton::clicked,this,[this]{connectHost(true);});connect(switchButton,&QPushButton::clicked,this,&Window::switchMode);
    connect(check,&QPushButton::clicked,this,&Window::checkEnvironment);connect(monitors,&QPushButton::clicked,this,&Window::listMonitors);
    connect(folder,&QPushButton::clicked,this,[this]{QDesktopServices::openUrl(QUrl::fromLocalFile(root));});
    connect(selectRuntime,&QPushButton::clicked,this,[this]{auto f=QFileDialog::getOpenFileName(this,"세션 엔진 명세 선택",{},"WayWin runtime (waywin-runtime.json)");if(!f.isEmpty()){runtime=f;save();refresh();}});
    connect(validation,&Validation::failure,this,&Window::showError);
    connect(latest,&QPushButton::clicked,this,[this]{auto h=selected();if(!h||!sessions.contains(h->id))return;validation->attachRun(sessions[h->id]->summary());tabs->setCurrentWidget(validation);});
    if(QSystemTrayIcon::isSystemTrayAvailable()) {
        auto tray=new QSystemTrayIcon(windowIcon(),this);auto menu=new QMenu(this);menu->addAction("WayWin 열기",this,[this]{show();raise();activateWindow();});menu->addAction("종료",this,&QWidget::close);tray->setContextMenu(menu);tray->setToolTip("WayWin");tray->show();
    }
    for(const auto &h:hosts)list->addItem(h.name);
    if(!hosts.isEmpty())list->setCurrentRow(0);
    checkEnvironment();refresh();
}
Host *Window::selected(){int i=list->currentRow();return i>=0&&i<hosts.size()?&hosts[i]:nullptr;}
void Window::showError(const QString &error){statusBar()->showMessage(error);QMessageBox::warning(this,"WayWin",error);}
bool Window::save(){if(!configWritable)return false;QJsonArray a;for(const auto &h:hosts)a.append(h.json());QString e;if(!saveJson(root+"/hosts.json",{{"schema",1},{"runtime",runtime},{"hosts",a}},&e)){showError(e);return false;}return true;}
void Window::refresh(){
    auto h=selected();auto s=h?sessions.value(h->id):nullptr;
    for(int i=0;i<hosts.size();++i){auto session=sessions.value(hosts[i].id);list->item(i)->setText(hosts[i].name+"\n"+(busy.contains(hosts[i].id)?"준비 중":session?session->state:"연결 안 됨"));QPixmap badge(12,12);badge.fill(QColor(hosts[i].accent()));list->item(i)->setIcon(QIcon(badge));}
    companion->setSession(s);
    title->setText(h?h->name:"호스트를 추가하세요");state->setText(s?s->state:"연결 안 됨");detail->setText(s?s->detail:"호스트 설정 후 연결을 누르세요.");
    const bool pending=h&&busy.contains(h->id),active=s&&s->active();
    connectButton->setEnabled(h&&!active&&!pending&&configWritable);disconnectButton->setEnabled(active);
    editButton->setEnabled(h&&!active&&!pending&&configWritable);deleteButton->setEnabled(h&&!active&&!pending&&configWritable);validation->setEnabled(h!=nullptr);
    const auto selectedItem=windows->currentItem();const auto selectedId=selectedItem?selectedItem->data(Qt::UserRole):QVariant();const auto selectedTray=selectedItem?selectedItem->data(Qt::UserRole+1):QVariant();windows->clear();
    if(s){for(auto id:s->windows){auto item=new QListWidgetItem(s->windowTitles.value(id,"창 ID "+QString::number(id)),windows);item->setData(Qt::UserRole,QVariant::fromValue(quint64(id)));item->setData(Qt::UserRole+1,false);}
        for(auto id:s->icons){auto item=new QListWidgetItem(s->trayTitles.value(id,"알림 아이콘 "+QString::number(id>>32)+":"+QString::number(quint32(id))),windows);item->setData(Qt::UserRole,QVariant::fromValue(id));item->setData(Qt::UserRole+1,true);auto bytes=s->trayImages.value(id);if(bytes.size()>36){const auto p=reinterpret_cast<const uchar*>(bytes.constData()+24);const auto w=WayWinWire::get(p+4),h=WayWinWire::get(p+8);QImage image(p+12,int(w),int(h),int(w*4),QImage::Format_ARGB32);item->setIcon(QPixmap::fromImage(image.copy()));}}}
    for(int i=0;i<windows->count();++i){auto item=windows->item(i);if(item->data(Qt::UserRole)==selectedId&&item->data(Qt::UserRole+1)==selectedTray)windows->setCurrentItem(item);}
    runtimeLabel->setText("세션 엔진: "+runtime);
}
void Window::editHost(bool create){
    if(!configWritable){showError("설정 파일을 먼저 복구하세요.");return;}
    auto current=selected();if(!create&&!current)return;
    Host h=create?Host{}:*current;if(create)h.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    const Host old=h;
    QDialog dialog(this);dialog.setWindowTitle(create?"호스트 추가":"호스트 설정");auto form=new QFormLayout(&dialog);
    QMap<QString,QLineEdit*> fields;
    for(const auto &pair:QList<QPair<QString,QString>>{{"name","표시 이름"},{"server","서버 주소"},{"user","Windows 사용자명"},{"domain","인증 도메인"},{"app","RemoteApp 별칭·경로"},{"monitors","SDL 화면 ID"}}){auto line=new QLineEdit(h.json()[pair.first].toString());line->setObjectName(pair.first);fields[pair.first]=line;form->addRow(pair.second,line);}
    fields["server"]->setPlaceholderText("192.168.0.10:3389 또는 rdp://host:3389");fields["app"]->setPlaceholderText("||WayWinProbe");fields["monitors"]->setPlaceholderText("비우면 전체 화면, 예: 2,1");
    QSpinBox width,height;width.setRange(200,8192);height.setRange(200,8192);width.setValue(h.width);height.setValue(h.height);form->addRow("창 너비",&width);form->addRow("창 높이",&height);
    QComboBox depth;for(int n:{16,24,32})depth.addItem(QString::number(n),n);depth.setCurrentIndex(depth.findData(h.depth));form->addRow("색 깊이",&depth);
    QCheckBox multi("전체 데스크톱도 다중 화면 사용"),dynamic("화면 변경 전달"),sound("원격 소리 재생"),remember("비밀번호를 OS 키링에 저장");multi.setChecked(h.multimon);dynamic.setChecked(h.dynamic);sound.setChecked(h.sound);remember.setChecked(h.remember);form->addRow(&multi);form->addRow(&dynamic);form->addRow(&sound);form->addRow(&remember);
    auto info=new QLabel("비밀번호는 연결 시 입력합니다. 클립보드는 Windows 앱·셸·클립보드 탭에서 선택한 호스트와만 수동 전송합니다.");info->setWordWrap(true);form->addRow(info);
    QDialogButtonBox buttons(QDialogButtonBox::Save|QDialogButtonBox::Cancel);form->addRow(&buttons);connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(&buttons,&QDialogButtonBox::accepted,&dialog,[&]{
        auto o=h.json();for(auto it=fields.begin();it!=fields.end();++it)o[it.key()]=it.value()->text().trimmed();h=Host::fromJson(o);
        h.width=width.value();h.height=height.value();h.depth=depth.currentData().toInt();h.multimon=multi.isChecked();h.dynamic=dynamic.isChecked();h.sound=sound.isChecked();h.remember=remember.isChecked();
        // Monitor selection is also valid when the next connection uses RAIL.
        if(!h.monitors.isEmpty()&&!h.multimon)h.mode="rail";
        auto error=h.validate();if(!error.isEmpty()){QMessageBox::warning(&dialog,"설정 확인",error);return;}dialog.accept();
    });
    if(dialog.exec()!=QDialog::Accepted)return;
    auto commit=[this,h,create]{auto before=hosts;int row=-1;for(int i=0;i<hosts.size();++i)if(hosts[i].id==h.id)row=i;if(create)hosts.append(h);else if(row>=0)hosts[row]=h;else return;if(!save()){hosts=before;return;}if(create){list->addItem(h.name);list->setCurrentRow(hosts.size()-1);}else{auto s=sessions.take(h.id);delete s;mode->setCurrentIndex(mode->findData(h.mode));}refresh();};
    if(!create&&old.remember&&(old.server!=h.server||old.user!=h.user||old.domain!=h.domain||!h.remember)) {
        busy.insert(h.id);refresh();
        deleteSecret(this,h.id,[this,commit,id=h.id](bool ok){busy.remove(id);if(ok)commit();else showError("이전 인증 정보를 키링에서 삭제하지 못했습니다. 잠금을 해제한 뒤 설정을 다시 변경하세요.");refresh();});
    } else commit();
}
void Window::removeHost(){auto h=selected();if(!h)return;const auto id=h->id;if(QMessageBox::question(this,"호스트 삭제",h->name+" 설정과 저장 비밀번호를 삭제할까요? 검증 기록은 보존됩니다.")!=QMessageBox::Yes)return;
    auto remove=[this,id](bool ok){if(!ok){showError("키링 삭제에 실패했습니다. 잠금을 해제하고 다시 시도하세요.");return;}int row=-1;for(int i=0;i<hosts.size();++i)if(hosts[i].id==id)row=i;if(row<0)return;auto before=hosts;hosts.removeAt(row);if(!save()){hosts=before;return;}delete sessions.take(id);delete list->takeItem(row);refresh();};
    if(h->remember)deleteSecret(this,id,remove);else remove(true);
}
void Window::checkEnvironment(){auto e=environment();QString text="환경: "+e["system"].toString()+"\n";const auto checks=e["checks"].toObject();for(auto it=checks.begin();it!=checks.end();++it)text+=(it.value().toBool()?"✓ ":"✕ ")+it.key()+"\n";text+=e["target_ready"].toBool()?"사전 조건 충족. 실제 연결 검증을 진행하세요.":"접속에는 Ubuntu 26.04 GNOME Wayland가 필요합니다. 이 PC에서도 설정·검증 기록 화면을 사용할 수 있습니다.";environmentLabel->setText(text);}
void Window::connectHost(bool reconnect){
    auto selectedHost=selected();if(!selectedHost||busy.contains(selectedHost->id))return;Host h=*selectedHost;h.mode=mode->currentData().toString();
    auto old=sessions.value(h.id);
    if(old&&old->active()){
        if(!reconnect)return;
        auto once=std::make_shared<QMetaObject::Connection>();*once=connect(old,&Session::ended,this,[this,h,once]{disconnect(*once);if(!closing)connectTarget(h,false);});old->stop();return;
    }
    bool launchApp=!reconnect&&launch->isChecked()&&h.mode=="rail";
    connectTarget(h,launchApp);
}
void Window::connectTarget(const Host&h,bool launchApp){
    auto error=h.validate();if(!error.isEmpty()){showError(error);return;}if(launchApp&&h.app.isEmpty()){showError("호스트 설정에 RemoteApp 별칭·경로를 입력하세요.");return;}
    if(!environment()["target_ready"].toBool()){tabs->setCurrentIndex(2);showError("실제 접속은 Ubuntu 26.04 GNOME Wayland에서 실행하세요.");return;}
    busy.insert(h.id);refresh();
    auto watcher=new QFutureWatcher<QPair<QString,QString>>(this);
    connect(watcher,&QFutureWatcherBase::finished,this,[this,watcher,h,launchApp]{auto result=watcher->result();watcher->deleteLater();if(closing){busy.remove(h.id);return;}if(result.first.isEmpty()){busy.remove(h.id);refresh();showError(result.second);return;}authenticate(h,result.first,launchApp);});
    watcher->setFuture(QtConcurrent::run([manifest=runtime]{QString error;auto binary=verifyRuntime(manifest,&error);return qMakePair(binary,error);}));
}
void Window::authenticate(const Host &h,const QString &binary,bool launchApp){
    auto prompt=[this,h,binary,launchApp]{
        bool ok=false;QString password=QInputDialog::getText(this,"Windows 인증",h.name+" · "+h.user+"\n비밀번호",QLineEdit::Password,{},&ok);
        if(!ok){busy.remove(h.id);refresh();return;}
        auto bytes=password.toUtf8();password.fill(QChar(0));
        if(bytes.contains('\n')||bytes.contains('\r')||bytes.contains('\0')||bytes.size()>1000){bytes.fill('\0');busy.remove(h.id);refresh();showError("비밀번호는 줄바꿈 없이 UTF-8 1000바이트 이내로 입력하세요.");return;}
        if(h.remember){auto job=new QKeychain::WritePasswordJob("WayWin",this);job->setKey(h.id);job->setInsecureFallback(false);job->setBinaryData(bytes);connect(job,&QKeychain::Job::finished,this,[this,job]{if(job->error()!=QKeychain::NoError)showError("키링 저장에 실패했습니다. 이번 연결에만 사용하며 다음 연결에서 다시 입력하세요.");});job->start();}
        begin(h,binary,bytes,launchApp);bytes.fill('\0');
    };
    if(!h.remember){prompt();return;}
    auto job=new QKeychain::ReadPasswordJob("WayWin",this);job->setKey(h.id);job->setInsecureFallback(false);
    connect(job,&QKeychain::Job::finished,this,[this,job,h,binary,launchApp,prompt]{if(closing){busy.remove(h.id);return;}if(job->error()==QKeychain::NoError){auto bytes=job->binaryData();begin(h,binary,bytes,launchApp);bytes.fill('\0');}else prompt();});job->start();
}
void Window::begin(const Host &h,const QString &binary,QByteArray password,bool launchApp){
    busy.remove(h.id);if(password.size()>1000||password.contains('\n')||password.contains('\r')||password.contains('\0')){password.fill('\0');showError("저장된 인증 정보 형식이 올바르지 않습니다. 저장 비밀번호를 삭제하고 다시 입력하세요.");refresh();return;}if(closing){password.fill('\0');return;}
    auto s=sessions.value(h.id);if(s&&s->active()){password.fill('\0');return;}
    if(!s){s=new Session(h,root,this);sessions[h.id]=s;connect(s,&Session::changed,this,&Window::refresh);
        connect(s,&Session::ended,this,[this]{refresh();if(closing)QTimer::singleShot(0,this,&QWidget::close);});
        connect(s,&Session::crashRecoveryRequested,this,[this,s]{if(!closing&&!s->active())connectTarget(s->host,false);});
        connect(s,&Session::closeRequested,this,[this,s]{if(QMessageBox::question(this,"연결 해제",s->host.name+"의 연결을 해제하고 원격 창을 닫을까요?") == QMessageBox::Yes)s->stop();});
    }
    s->host=h;s->start(binary,password,launchApp);password.fill('\0');refresh();
}
void Window::switchMode(){
    auto h=selected();if(!h)return;auto s=sessions.value(h->id);if(s&&s->active()){
        if(QMessageBox::question(this,"모드 전환","현재 연결을 해제한 뒤 앱 실행 없이 다른 모드로 다시 연결합니다. Windows 정책에 따른 세션·작업 보존을 검증하세요. 계속할까요?")!=QMessageBox::Yes)return;
        mode->setCurrentIndex(s->host.mode=="desktop"?1:0);
    }else mode->setCurrentIndex(mode->currentIndex()==0?1:0);
    launch->setChecked(false);connectHost(true);
}
void Window::listMonitors(){
    if(!environment()["target_ready"].toBool()){showError("화면 ID 조회는 대상 Ubuntu Wayland에서 실행하세요.");return;}
    auto watcher=new QFutureWatcher<QPair<QString,QString>>(this);
    connect(watcher,&QFutureWatcherBase::finished,this,[this,watcher]{auto r=watcher->result();watcher->deleteLater();if(r.first.isEmpty()){showError(r.second);return;}
        auto p=new QProcess(this);p->setProcessEnvironment(sessionEnvironment({}));p->setStandardErrorFile(QProcess::nullDevice());
        connect(p,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,p](int code,QProcess::ExitStatus){auto out=p->readAllStandardOutput().left(16384);p->deleteLater();if(code!=0)showError("화면 ID 조회 실패. Wayland 환경과 엔진 설치를 확인하세요.");else QMessageBox::information(this,"SDL 화면 ID",QString::fromUtf8(out));});
        connect(p,&QProcess::errorOccurred,this,[this,p](QProcess::ProcessError e){if(e==QProcess::FailedToStart){showError("엔진 실행 실패");p->deleteLater();}});
        auto timer=new QTimer(p);timer->setSingleShot(true);connect(timer,&QTimer::timeout,p,&QProcess::kill);timer->start(10000);p->start(r.first,{"/list:monitor"});
    });watcher->setFuture(QtConcurrent::run([manifest=runtime]{QString e;auto b=verifyRuntime(manifest,&e);return qMakePair(b,e);}));
}
void Window::closeEvent(QCloseEvent *event){
    bool active=false;for(auto s:sessions)active|=s->active();
    if(active){
        if(!closing&&QMessageBox::question(this,"WayWin 종료","모든 원격 연결을 해제하고 종료할까요?")!=QMessageBox::Yes){event->ignore();return;}
        closing=true;for(auto s:sessions)s->stop();event->ignore();return;
    }
    if(!validation->flush()){event->ignore();return;}closing=true;event->accept();
}
}
