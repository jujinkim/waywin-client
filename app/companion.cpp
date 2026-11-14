// SPDX-License-Identifier: Apache-2.0
#include "companion.h"
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QBuffer>
#include <QImageReader>
#include <QWheelEvent>
#include <QSet>
#include <QSocketNotifier>
#include <unistd.h>
#include <csignal>
using namespace WayWinWire;
namespace WayWin {
class Surface : public QWidget {
    QPointer<Session> session;
    uint32_t id;
    uint32_t epoch;
    QImage frame;
    QRect content;
    QSet<uint32_t> held;
    void pointer(QPoint point,uint32_t action) {
        if(!session||session->epoch!=epoch||frame.isNull()||!content.contains(point))return;
        std::vector<uint8_t> data;put(data,id);
        put(data,uint32_t((int64_t(point.x()-content.x())*65535)/std::max(1,content.width()-1)));
        put(data,uint32_t((int64_t(point.y()-content.y())*65535)/std::max(1,content.height()-1)));
        put(data,action);session->request(Pointer,data);
    }
    static uint32_t virtualKey(int key) {
        if(key>=Qt::Key_A&&key<=Qt::Key_Z)return uint32_t(key);
        if(key>=Qt::Key_0&&key<=Qt::Key_9)return uint32_t(key);
        if(key>=Qt::Key_F1&&key<=Qt::Key_F24)return uint32_t(0x70+key-Qt::Key_F1);
        switch(key){case Qt::Key_Backspace:return 8;case Qt::Key_Tab:return 9;case Qt::Key_Return:case Qt::Key_Enter:return 13;case Qt::Key_Shift:return 16;case Qt::Key_Control:return 17;case Qt::Key_Alt:return 18;case Qt::Key_Escape:return 27;case Qt::Key_Space:return 32;case Qt::Key_PageUp:return 33;case Qt::Key_PageDown:return 34;case Qt::Key_End:return 35;case Qt::Key_Home:return 36;case Qt::Key_Left:return 37;case Qt::Key_Up:return 38;case Qt::Key_Right:return 39;case Qt::Key_Down:return 40;case Qt::Key_Insert:return 45;case Qt::Key_Delete:return 46;default:return 0;}
    }
    void key(uint32_t vk,bool down){if(!session||session->epoch!=epoch)return;std::vector<uint8_t>d;put(d,id);put(d,vk);put(d,down?1:0);session->request(Key,d);if(down)held.insert(vk);else held.remove(vk);}
    void text(const QString&s){if(!session||session->epoch!=epoch||s.isEmpty())return;std::vector<uint8_t>d;put(d,id);string(d,s.toUtf8().toStdString());session->request(Text,d);}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.fillRect(rect(),QColor("#171923"));
        if(frame.isNull()){p.setPen(Qt::white);p.drawText(rect(),Qt::AlignCenter,"원격 셸 화면을 기다리고 있습니다.");return;}
        auto size=frame.size().scaled(this->size(),Qt::KeepAspectRatio);content=QRect(QPoint((width()-size.width())/2,(height()-size.height())/2),size);p.drawImage(content,frame);
        p.fillRect(QRect(0,0,width(),3),QColor(qEnvironmentVariable("WAYWIN_ACCENT","#7651c9")));
        if(!session||session->state!="연결됨"){p.fillRect(rect(),QColor(0,0,0,170));p.setPen(Qt::white);p.drawText(rect(),Qt::AlignCenter,"연결 복구 중 · 입력 잠김");}
    }
    void mouseMoveEvent(QMouseEvent*e)override{pointer(e->position().toPoint(),0);}
    void mousePressEvent(QMouseEvent*e)override{setFocus();pointer(e->position().toPoint(),e->button()==Qt::RightButton?3:1);}
    void mouseReleaseEvent(QMouseEvent*e)override{auto p=e->position().toPoint();if(content.isValid()){p.setX(qBound(content.left(),p.x(),content.right()));p.setY(qBound(content.top(),p.y(),content.bottom()));}pointer(p,e->button()==Qt::RightButton?4:2);}
    void wheelEvent(QWheelEvent*e)override{pointer(e->position().toPoint(),e->angleDelta().y()>0?5:6);}
    void keyPressEvent(QKeyEvent*e)override{
        if(e->isAutoRepeat())return;
        auto vk=virtualKey(e->key());
        if(!e->text().isEmpty()&&!(e->modifiers()&(Qt::ControlModifier|Qt::AltModifier))&&e->key()>=Qt::Key_Space&&e->key()<Qt::Key_Escape)text(e->text());
        else if(vk)key(vk,true);
        e->accept();
    }
    void keyReleaseEvent(QKeyEvent*e)override{auto vk=virtualKey(e->key());if(!e->isAutoRepeat()&&held.contains(vk))key(vk,false);e->accept();}
    void inputMethodEvent(QInputMethodEvent*e)override{text(e->commitString());e->accept();}
    void focusOutEvent(QFocusEvent*)override{for(auto vk:QSet<uint32_t>(held))key(vk,false);}
    void closeEvent(QCloseEvent*e)override{if(session&&session->epoch==epoch){for(auto vk:QSet<uint32_t>(held))key(vk,false);std::vector<uint8_t>d;put(d,id);session->request(SurfaceClose,d);}e->accept();}
public:
    Surface(Session*s,uint32_t surface,const QString&name):session(s),id(surface),epoch(s->epoch) {
        setAttribute(Qt::WA_DeleteOnClose);setAttribute(Qt::WA_InputMethodEnabled);setFocusPolicy(Qt::StrongFocus);setMouseTracking(true);
        setWindowTitle(s->host.name+" · "+name);resize(800,650);setMinimumSize(240,180);
        connect(s,&Session::changed,this,qOverload<>(&QWidget::update));
        connect(s,&Session::companionChanged,this,[this]{if(session&&session->epoch!=epoch)close();});
        connect(s,&Session::ended,this,&QWidget::close);
        connect(s,&QObject::destroyed,this,&QWidget::close);
    }
    bool updateFrame(const QByteArray&bytes) {
        QBuffer buffer;buffer.setData(bytes);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer,"JPEG");
        const auto size=reader.size();if(!size.isValid()||size.width()>1600||size.height()>8192||qint64(size.width())*size.height()>1600*8192)return false;
        auto image=reader.read();if(image.isNull())return false;frame=std::move(image);update();return true;
    }
};
Companion::Companion(QWidget*parent):QWidget(parent) {
    auto layout=new QVBoxLayout(this);status=new QLabel("Windows 보조 앱을 기다리고 있습니다.");status->setWordWrap(true);status->setTextFormat(Qt::PlainText);layout->addWidget(status);
    auto guide=new QLabel("Windows에서 WayWin Companion.exe를 실행하고 ‘Install + start at sign-in’을 누르세요. 같은 RDP 세션에서 연결되면 아래 기능을 사용할 수 있습니다.");guide->setWordWrap(true);layout->addWidget(guide);
    actions=new QWidget;auto body=new QVBoxLayout(actions);layout->addWidget(actions);
    auto shell=new QGroupBox("Windows 원본 화면");auto shellLayout=new QHBoxLayout(shell);body->addWidget(shell);
    int kind=0;for(const auto&label:QStringList{"시작 메뉴","입력기 설정","빠른 설정","IME 후보 창"}){auto button=new QPushButton(label);const auto id=++kind;shellLayout->addWidget(button);connect(button,&QPushButton::clicked,this,[this,id]{if(session){std::vector<uint8_t>d;put(d,id);session->request(WayWinWire::Shell,d);status->setText("Windows 원본 화면을 요청했습니다…");}});}
    auto appBox=new QGroupBox("이 Windows 세션의 앱");auto appLayout=new QVBoxLayout(appBox);body->addWidget(appBox);apps=new QListWidget;apps->setObjectName("remoteApps");appLayout->addWidget(apps);
    auto appButtons=new QHBoxLayout;appLayout->addLayout(appButtons);auto reload=new QPushButton("앱 목록 새로고침");auto launch=new QPushButton("선택 앱 실행");appButtons->addWidget(reload);appButtons->addWidget(launch);
    connect(reload,&QPushButton::clicked,this,[this]{if(session)session->request(Apps);});
    connect(launch,&QPushButton::clicked,this,[this]{if(!session||!apps->currentItem())return;std::vector<uint8_t>d;put(d,apps->currentItem()->data(Qt::UserRole).toUInt());session->request(Launch,d);});
    auto clipBox=new QGroupBox("선택 호스트의 텍스트 클립보드");auto clipLayout=new QVBoxLayout(clipBox);body->addWidget(clipBox);clipboard=new QPlainTextEdit;clipboard->setPlaceholderText("최대 UTF-8 64 KiB · 선택한 호스트와만 전송");clipboard->setMaximumHeight(130);clipLayout->addWidget(clipboard);
    auto clipButtons=new QHBoxLayout;clipLayout->addLayout(clipButtons);
    auto import=new QPushButton("이 PC에서 가져오기"),get=new QPushButton("Windows에서 읽기"),send=new QPushButton("Windows로 보내기"),copy=new QPushButton("이 PC로 복사");
    for(auto b:{import,get,send,copy})clipButtons->addWidget(b);
    connect(import,&QPushButton::clicked,this,[this]{clipboard->setPlainText(QApplication::clipboard()->text().left(65536));});
    connect(copy,&QPushButton::clicked,this,[this]{QApplication::clipboard()->setText(clipboard->toPlainText());});
    connect(get,&QPushButton::clicked,this,[this]{if(session)session->request(ClipboardGet);});
    connect(send,&QPushButton::clicked,this,[this]{auto bytes=clipboard->toPlainText().toUtf8();if(bytes.size()>65536){status->setText("텍스트가 64 KiB를 초과합니다.");return;}if(session){std::vector<uint8_t>d;string(d,bytes.toStdString());session->request(ClipboardSet,d);}});
    auto info=new QLabel("클립보드는 버튼을 눌렀을 때만 전송합니다. 원격 화면·앱 이름·클립보드 내용은 진단 기록에 저장하지 않습니다.");info->setWordWrap(true);body->addWidget(info);
    refresh();
}
void Companion::setSession(Session*s) {
    if(session==s){refresh();return;}
    if(session){clipboardDrafts[session->host.id]=clipboard->toPlainText();disconnect(session,&Session::companionChanged,this,&Companion::refresh);}
    session=s;apps->clear();clipboard->setPlainText(s?clipboardDrafts.value(s->host.id):QString());
    if(s){
        connect(s,&Session::companionChanged,this,&Companion::refresh,Qt::UniqueConnection);
        if(!surfaces.contains(s)) {
            surfaces[s]={};connect(s,&Session::transient,this,[this,s](const QByteArray&b){message(s,b);});
            connect(s,&QObject::destroyed,this,[this,s]{surfaces.remove(s);});
        }
    }
    refresh();
}
void Companion::refresh(){actions->setEnabled(session&&session->companionReady&&session->state=="연결됨");status->setText(session?session->companionStatus:"연결할 호스트를 선택하세요.");}
void Companion::message(Session*s,const QByteArray&bytes) {
    try {
        Decoder decoder;auto messages=decoder.feed(reinterpret_cast<const uint8_t*>(bytes.constData()),size_t(bytes.size()));if(messages.size()!=1)return;auto&m=messages[0];Reader read(m.data);
        if(m.type==AppList&&session==s){auto count=read.number();if(count>1000)return;apps->clear();for(uint32_t i=0;i<count;++i){auto id=read.number();auto item=new QListWidgetItem(QString::fromStdString(read.text(4096)),apps);item->setData(Qt::UserRole,id);}}
        else if(m.type==Clipboard){auto value=QString::fromStdString(read.text());clipboardDrafts[s->host.id]=value;if(session==s)clipboard->setPlainText(value);}
        else if(m.type==Frame){
            auto kind=read.number();read.number();read.number();if(kind<1||kind>4)return;
            auto current=surfaces[s].value(m.request);auto surface=static_cast<Surface*>(current.data());
            if(!surface){if(surfaces[s].contains(m.request))return;int live=0;for(auto w:surfaces[s])if(w)++live;if(live>=8)return;surface=new Surface(s,m.request,QStringList{"시작 메뉴","입력기 설정","빠른 설정","IME 후보 창"}.at(int(kind)-1));surfaces[s][m.request]=surface;}
            const auto image=QByteArray(reinterpret_cast<const char*>(m.data.data()+12),qsizetype(m.data.size()-12));if(surface->updateFrame(image)&&!surface->isVisible())surface->show();
        }else if(m.type==SurfaceGone){if(auto surface=surfaces[s].take(m.request))surface->close();}
    }catch(...){if(session==s)status->setText("보조 앱 데이터가 올바르지 않습니다. 버전을 확인하세요.");}
}
int runSurfaces(const QString&name) {
    std::signal(SIGPIPE,SIG_IGN);QApplication::setQuitOnLastWindowClosed(false);
    Host host;host.name=name;Session proxy(host,{});proxy.surfaceProxy=true;proxy.companionReady=true;proxy.state="연결됨";
    Companion panel;panel.setSession(&proxy);Decoder decoder;QSocketNotifier input(STDIN_FILENO,QSocketNotifier::Read);
    QObject::connect(&input,&QSocketNotifier::activated,&panel,[&]{
        uint8_t buffer[65536];auto n=::read(STDIN_FILENO,buffer,sizeof(buffer));if(n<=0){QCoreApplication::quit();return;}
        try{for(const auto&m:decoder.feed(buffer,size_t(n))){
            if(m.epoch<proxy.epoch)continue;
            if(m.epoch!=proxy.epoch){proxy.epoch=m.epoch;emit proxy.companionChanged();}
            if(m.type==Retry){Reader r(m.data);proxy.state=r.number()==0?"연결됨":"재연결 중";emit proxy.changed();}
            else if(m.type==Channel){Reader r(m.data);proxy.state=r.number()?"연결됨":"재연결 중";emit proxy.changed();}
            else if(m.type==Frame||m.type==SurfaceGone){auto bytes=encode(m);emit proxy.transient(QByteArray(reinterpret_cast<const char*>(bytes.data()),qsizetype(bytes.size())));}
        }}catch(...){QCoreApplication::exit(2);}
    });
    return QCoreApplication::exec();
}
}
