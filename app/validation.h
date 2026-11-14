// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core.h"
#include <QWidget>
#include <QMap>
class QTreeWidget;
class QLineEdit;
class QComboBox;
class QPlainTextEdit;
class QLabel;
class QCheckBox;
namespace WayWin {
class Validation : public QWidget {
    Q_OBJECT
public:
    explicit Validation(QWidget *parent=nullptr);
    void setHost(const QString &path);
    void attachRun(const QJsonObject &summary);
    QJsonObject report() const {return data;}
    bool flush();
signals:
    void failure(const QString &message);
private:
    QJsonObject data;
    QString path,group,key;
    bool loading=false;
    QTreeWidget *tree;
    QLabel *instructions,*saved;
    QLineEdit *tester,*date,*evidence;
    QPlainTextEdit *notes,*apps;
    QComboBox *status;
    QMap<QString,QLineEdit*> environmentFields,layouts;
    QMap<QString,QCheckBox*> shell;
    void selectCase();
    void update();
    void load();
    void exportReport();
};
}
