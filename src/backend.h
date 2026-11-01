// SPDX-FileCopyrightText: 2026 Nuaptan Evalisk
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <QJsonArray>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QTimer>
#include <atomic>
struct Device {
    QString name, os, ip, reason;
    bool online = false, eligible = false;
};
QList<Device> parseDevices(const QByteArray &json, QString &error);
class Backend : public QObject {
    Q_OBJECT
  public:
    explicit Backend(QObject *parent = nullptr, QString cli = {});
    ~Backend();
    QList<Device> devices;
    QString destination, stagingRoot;
    QJsonArray history;
    bool paused = false, sending = false;
    void start();
    void refresh();
    void setPaused(bool);
    void receive();
    void send(const QStringList &paths, const Device &device);
    void cancelSend();
    void retry();
    void clearHistory();
    void stop();
  signals:
    void devicesChanged();
    void recoveryStatus(QString);
    void status(QString);
    void sendStatus(QString, bool);
    void received(QStringList);
    void historyChanged();
    void sendFailed(QString);

  private:
    std::atomic_bool stopPublish{false};
    QSet<QString> publishing;
    QString executable, batch, target, targetName;
    QString stagingError, lastSendError;
    QStringList pending, failed;
    int total = 0, completed = 0, delay = 5000;
    bool quitting = false, cancelled = false, supported = false;
    QProcess receiver, sender, query;
    QTimer retryTimer, refreshTimer;
    void nextSend();
    void record(QString direction, QString path, QString peer, QString result);
    void saveHistory();
    void finishReceive(int code);
    void publishBatch();
    void runProbe(int step = 0);
    void scanRecovery();
};
