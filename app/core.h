// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStringList>
#include <QVector>
namespace WayWin {
struct Host {
    QString id, name, server, user, domain, app, monitors;
    QString mode = "desktop";
    QString displayBackend = "auto";
    int width = 1280, height = 720, depth = 32;
    bool multimon = false, dynamic = false, sound = false, remember = false;
    QJsonObject json() const;
    static Host fromJson(const QJsonObject &o);
    QString validate() const;
    QString accent() const;
};
QString endpoint(const QString &input, QString *error = nullptr);
QStringList arguments(const Host &host, bool launch);
QStringList displayDrivers(const QProcessEnvironment &environment, const QString &preference = "auto");
QProcessEnvironment sessionEnvironment(const QString &certificateDirectory, const QString &preference = "auto");
QJsonObject environment(const QString &preference = "auto");
bool saveJson(const QString &path, const QJsonObject &object, QString *error = nullptr);
QJsonObject readJson(const QString &path, QString *error = nullptr);
QString privateDirectory(const QString &path);
QString verifyRuntime(const QString &manifest, QString *error, QJsonObject *metadata = nullptr);
QJsonObject parseEvent(const QByteArray &line);
class EventDecoder {
public:
    QList<QJsonObject> feed(const QByteArray &chunk);
private:
    QByteArray pending;
    bool dropping = false;
};
QJsonObject newReport();
QJsonObject migrateReport(QJsonObject report);
QStringList assessReport(const QJsonObject &report);
QString caseInstructions(const QString &id);
}
