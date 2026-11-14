// SPDX-License-Identifier: Apache-2.0
#include "validation.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace WayWin {
Validation::Validation(QWidget *parent):QWidget(parent),data(newReport()) {
    auto root=new QVBoxLayout(this);
    auto intro=new QLabel("이 제품의 실제 연결을 사용해 검증합니다. 상태는 관측 후 직접 기록하세요. 자동 환경 점검과 이벤트 수신만으로 합격 처리하지 않습니다.");
    intro->setWordWrap(true);root->addWidget(intro);
    auto tabs=new QTabWidget;root->addWidget(tabs);
    auto cases=new QWidget;auto casesLayout=new QVBoxLayout(cases);
    auto split=new QSplitter;casesLayout->addWidget(split);
    tree=new QTreeWidget;tree->setHeaderLabels({"검증 항목","결과"});tree->setMinimumWidth(245);split->addWidget(tree);
    const QMap<QString,QString> groups{{"cases","기능 P01–P10"},{"shell_surfaces","원본 Windows 셸"},{"monitor_matrix","화면 구성 M01–M06"}};
    for(const auto &g:QStringList{"cases","shell_surfaces","monitor_matrix"}) {
        auto section=new QTreeWidgetItem(tree,{groups[g]});
        for(const auto &id:data[g].toObject().keys()) {
            auto item=new QTreeWidgetItem(section,{id,"미실행"});item->setData(0,Qt::UserRole,g);item->setData(0,Qt::UserRole+1,id);
        }
        section->setExpanded(true);
    }
    tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    auto edit=new QWidget;auto form=new QFormLayout(edit);split->addWidget(edit);split->setStretchFactor(1,1);
    instructions=new QLabel;instructions->setWordWrap(true);instructions->setTextInteractionFlags(Qt::TextSelectableByMouse);form->addRow(instructions);
    status=new QComboBox;status->addItem("미실행","not-run");status->addItem("차단됨","blocked");status->addItem("실패","fail");status->addItem("합격 관측","pass");form->addRow("결과",status);
    notes=new QPlainTextEdit;notes->setPlaceholderText("수행 동작, 실제 결과, Windows Session/PID/Instance, 배율, 실패 원인을 기록하세요. 비밀번호·개인 문서는 넣지 마세요.");form->addRow("관측",notes);
    evidence=new QLineEdit;evidence->setPlaceholderText("실행 UUID 또는 증거 참조 (세미콜론으로 구분)");form->addRow("증거",evidence);
    for(const auto &pair:QList<QPair<QString,QString>>{{"remote_ui_visible","원본 UI 별도 표시"},{"interactive","입력·조작 성공"},{"same_session","같은 사용자 세션"}}) {
        auto check=new QCheckBox(pair.second);shell[pair.first]=check;form->addRow(check);connect(check,&QCheckBox::toggled,this,[this]{update();});
    }
    for(const auto &pair:QList<QPair<QString,QString>>{{"host_physical_layout","Windows 물리 화면"},{"client_layout","Ubuntu 화면"},{"rdp_session_layout","RDP 세션 화면"}}) {
        auto line=new QLineEdit;line->setPlaceholderText("개수·원점·크기·배율·회전·주 화면");layouts[pair.first]=line;form->addRow(pair.second,line);connect(line,&QLineEdit::textChanged,this,[this]{update();});
    }
    tabs->addTab(cases,"검증 항목");
    auto context=new QWidget;auto ctx=new QFormLayout(context);
    tester=new QLineEdit;date=new QLineEdit;ctx->addRow("검증자",tester);ctx->addRow("날짜 (YYYY-MM-DD)",date);
    const QMap<QString,QString> labels{{"ubuntu","Ubuntu 버전"},{"gnome","GNOME 버전"},{"sdl_driver","실제 SDL 드라이버"},{"windows_edition_build","Windows 에디션·빌드"},{"rdp_policy","RDP·RemoteApp 정책"},{"freerdp_commit","FreeRDP 커밋"},{"patch_sha256","관측 패치 SHA256"}};
    for(const auto &k:data["environment"].toObject().keys()) {auto line=new QLineEdit;environmentFields[k]=line;ctx->addRow(labels.value(k,k),line);connect(line,&QLineEdit::textChanged,this,[this]{update();});}
    apps=new QPlainTextEdit;apps->setPlaceholderText("한 줄에 앱 이름 | 버전. 최소 5개.");ctx->addRow("대표 앱",apps);
    auto scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setWidget(context);tabs->addTab(scroll,"환경·앱 기록");
    auto actions=new QHBoxLayout;root->addLayout(actions);
    auto assess=new QPushButton("기록 완결성 확인");auto exportButton=new QPushButton("보고서 내보내기…");auto importButton=new QPushButton("보고서 가져오기…");
    actions->addWidget(assess);actions->addWidget(exportButton);actions->addWidget(importButton);actions->addStretch();saved=new QLabel;actions->addWidget(saved);
    connect(assess,&QPushButton::clicked,this,[this]{update();auto errors=assessReport(data);QMessageBox::information(this,"기록 완결성",errors.isEmpty()?"필수 기록이 갖추어졌습니다. 수동 관측 기록이며 독립적인 사실 검증이나 제품 합격 판정은 아닙니다.":errors.join('\n'));});
    connect(exportButton,&QPushButton::clicked,this,&Validation::exportReport);
    connect(importButton,&QPushButton::clicked,this,[this]{
        auto file=QFileDialog::getOpenFileName(this,"보고서 가져오기",{},"JSON (*.json)");if(file.isEmpty())return;
        QString error;auto r=readJson(file,&error);if(!error.isEmpty()||r["schema"].toInt()!=2){emit failure("schema 2 보고서를 선택하세요.");return;}
        if(QMessageBox::question(this,"현재 보고서 교체","선택 호스트의 검증 기록을 가져온 보고서로 교체할까요?")!=QMessageBox::Yes)return;
        data=r;data["independently_verified"]=false;load();flush();
    });
    connect(tree,&QTreeWidget::currentItemChanged,this,[this]{selectCase();});
    connect(status,&QComboBox::currentIndexChanged,this,[this]{update();});
    connect(notes,&QPlainTextEdit::textChanged,this,[this]{update();});
    connect(evidence,&QLineEdit::textChanged,this,[this]{update();});
    connect(tester,&QLineEdit::textChanged,this,[this]{update();});connect(date,&QLineEdit::textChanged,this,[this]{update();});connect(apps,&QPlainTextEdit::textChanged,this,[this]{update();});
    load();tree->setCurrentItem(tree->topLevelItem(0)->child(0));
}
void Validation::setHost(const QString &p) {
    if(p==path)return;
    if(!flush())return;
    path=p;data=newReport();
    if(QFileInfo::exists(path)) {QString error;auto r=readJson(path,&error);if(error.isEmpty()&&r["schema"].toInt()==2)data=r;else {emit failure("기존 검증 기록을 읽지 못했습니다. 원본 파일을 보존하고 내보내기로 별도 저장하세요.");path.clear();}}
    load();
}
void Validation::load() {
    loading=true;
    tester->setText(data["tester"].toString());date->setText(data["date"].toString());
    for(auto it=environmentFields.begin();it!=environmentFields.end();++it)it.value()->setText(data["environment"].toObject()[it.key()].toString());
    QStringList lines;for(const auto &v:data["applications"].toArray()){auto o=v.toObject();lines<<o["name"].toString()+" | "+o["version"].toString();}apps->setPlainText(lines.join('\n'));
    loading=false;selectCase();
    for(int i=0;i<tree->topLevelItemCount();++i)for(int j=0;j<tree->topLevelItem(i)->childCount();++j){auto item=tree->topLevelItem(i)->child(j);auto s=data[item->data(0,Qt::UserRole).toString()].toObject()[item->text(0)].toObject()["status"].toString("not-run");item->setText(1,status->itemText(status->findData(s)));}
}
void Validation::selectCase() {
    auto item=tree->currentItem();if(!item || item->data(0,Qt::UserRole).isNull())return;
    loading=true;group=item->data(0,Qt::UserRole).toString();key=item->data(0,Qt::UserRole+1).toString();
    auto o=data[group].toObject()[key].toObject();
    instructions->setText(key+" · "+o["description"].toString()+"\n\n"+caseInstructions(key));
    status->setCurrentIndex(qMax(0,status->findData(o["status"].toString())));notes->setPlainText(o["notes"].toString());
    QStringList refs;for(const auto &v:o["evidence"].toArray())refs<<v.toString();evidence->setText(refs.join("; "));
    for(auto it=shell.begin();it!=shell.end();++it){it.value()->setEnabled(group=="shell_surfaces");it.value()->setChecked(o[it.key()].toBool());}
    for(auto it=layouts.begin();it!=layouts.end();++it){it.value()->setEnabled(group=="monitor_matrix");it.value()->setText(o[it.key()].toString());}
    loading=false;
}
void Validation::update() {
    if(loading)return;
    data["tester"]=tester->text();data["date"]=date->text();data["independently_verified"]=false;
    QJsonObject env;for(auto it=environmentFields.begin();it!=environmentFields.end();++it)env[it.key()]=it.value()->text();data["environment"]=env;
    QJsonArray inventory;for(const auto &line:apps->toPlainText().split('\n',Qt::SkipEmptyParts)){auto parts=line.split('|');inventory.append(QJsonObject{{"name",parts.value(0).trimmed()},{"version",parts.value(1).trimmed()}});}data["applications"]=inventory;
    if(!key.isEmpty()){
        auto items=data[group].toObject();auto o=items[key].toObject();o["status"]=status->currentData().toString();o["notes"]=notes->toPlainText();
        QJsonArray refs;for(auto ref:evidence->text().split(';',Qt::SkipEmptyParts)){ref=ref.trimmed();if(!ref.isEmpty())refs.append(ref);}o["evidence"]=refs;
        if(group=="shell_surfaces")for(auto it=shell.begin();it!=shell.end();++it)o[it.key()]=it.value()->isChecked();
        if(group=="monitor_matrix")for(auto it=layouts.begin();it!=layouts.end();++it)o[it.key()]=it.value()->text();
        items[key]=o;data[group]=items;
        auto item=tree->currentItem();if(item && item->text(0)==key)item->setText(1,status->currentText());
    }
    flush();
}
bool Validation::flush() {
    if(path.isEmpty())return true;
    QString error;if(!saveJson(path,data,&error)){saved->setText("저장 실패");emit failure(error);return false;}saved->setText("자동 저장됨");return true;
}
void Validation::attachRun(const QJsonObject &run) {
    const auto id=run["run_id"].toString();if(id.isEmpty())return;
    auto refs=evidence->text().split(';',Qt::SkipEmptyParts);const auto ref="runs/"+id;
    if(!refs.contains(ref))refs<<ref;evidence->setText(refs.join(';'));
    if(run["sdl_driver_observed"].toString()=="wayland")environmentFields["sdl_driver"]->setText("wayland");
    update();
}
void Validation::exportReport() {
    update();auto file=QFileDialog::getSaveFileName(this,"검증 보고서 내보내기","waywin-validation.json","JSON (*.json)");if(file.isEmpty())return;
    QString error;if(!saveJson(file,data,&error))emit failure(error);
}
}
