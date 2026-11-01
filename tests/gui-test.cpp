// SPDX-FileCopyrightText: 2026 Nuaptan Evalisk
// SPDX-License-Identifier: GPL-3.0-only

#include "window.h"
#include <KNotification>
#include <KStatusNotifierItem>
#include <QApplication>
#include <QAbstractButton>
#include <QDesktopServices>
#include <QDBusConnection>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

class NotificationServer : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    uint count = 0;
    QStringList lastActions;
public slots:
    QStringList GetCapabilities() { return {"actions", "body"}; }
    QString GetServerInformation(QString &vendor, QString &version, QString &spec) {
        vendor = "TailCourier tests"; version = "1"; spec = "1.2"; return "Test server";
    }
    uint Notify(const QString &, uint, const QString &, const QString &, const QString &,
                const QStringList &actions, const QVariantMap &, int) {
        lastActions = actions;
        return ++count;
    }
    void CloseNotification(uint id) { emit NotificationClosed(id, 3); }
signals:
    void NotificationClosed(uint id, uint reason);
    void ActionInvoked(uint id, const QString &action);
};

class GuiTest : public QObject {
    Q_OBJECT
    QTemporaryDir temp;
    QUrl opened;
    NotificationServer notifications;
  public slots:
    void capture(const QUrl &url) {
        opened = url;
    }
  private slots:
    void initTestCase() {
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerService("org.freedesktop.Notifications"));
        QVERIFY(bus.registerObject("/org/freedesktop/Notifications", &notifications,
                                   QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName("tailcourier-gui-test");
        QFile f(temp.path() + "/tailscale");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("#!/bin/sh\nexit 1\n");
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("PATH", temp.path().toUtf8());
        QDesktopServices::setUrlHandler("file", this, "capture");
    }
    void windowAndTray() {
        Window w;
        w.show();
        QTest::qWait(100);
        QVERIFY(w.isVisible());
        auto tray = w.findChild<KStatusNotifierItem *>();
        QVERIFY(tray);
        QStringList labels;
        for (auto a : tray->contextMenu()->actions())
            labels << a->text();
        for (auto s : {"Devices", "Send Files…", "Transfer History", "Settings…", "Pause Automatic Receiving", "Quit"})
            QVERIFY(labels.contains(s));
        auto path = temp.path() + "/中文 ' 文件.txt";
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("safe");
        f.close();
        w.acceptArguments({"tailcourier", "--send", "--", path}, temp.path());
        bool selected = false;
        for (auto label : w.findChildren<QLabel *>())
            selected |= label->text().contains("1 file selected");
        QVERIFY(selected);
        w.grab().save("gui-smoke.png");
        w.close();
        QVERIFY(!w.isVisible());
    }
    void notificationActions() {
        Window w;
        auto p = temp.path() + "/received.txt";
        const auto before = notifications.count;
        emit w.backend.received({p});
        QTRY_COMPARE_WITH_TIMEOUT(notifications.count, before + 1, 5000);
        const auto actions = notifications.lastActions;
        const auto folderIndex = actions.indexOf("Open Containing Folder");
        const auto fileIndex = actions.indexOf("Open File");
        QVERIFY(folderIndex > 0);
        QVERIFY(fileIndex > 0);
        emit notifications.ActionInvoked(notifications.count, actions[folderIndex - 1]);
        QTRY_COMPARE(opened, QUrl::fromLocalFile(temp.path()));

        // A new notification is needed because invoking an action closes the previous one.
        emit w.backend.received({p});
        QTRY_COMPARE_WITH_TIMEOUT(notifications.count, before + 2, 5000);
        QTimer answer;
        connect(&answer, &QTimer::timeout, this, [] {
            for (auto widget : QApplication::topLevelWidgets())
                if (auto box = qobject_cast<QMessageBox *>(widget); box && box->isVisible())
                    box->button(QMessageBox::Yes)->click();
        });
        answer.start(10);
        emit notifications.ActionInvoked(notifications.count, notifications.lastActions[fileIndex - 1]);
        QTRY_COMPARE(opened, QUrl::fromLocalFile(p));
    }
    void batchNotifications() {
        Window w;
        const auto before = notifications.count;
        emit w.backend.received({temp.path() + "/a"});
        emit w.backend.received({temp.path() + "/b"});
        QTRY_COMPARE_WITH_TIMEOUT(notifications.count, before + 1, 5000);
        QVERIFY(notifications.lastActions.contains("Open Containing Folder"));
        QVERIFY(!notifications.lastActions.contains("Open File"));
    }
    void cleanupTestCase() {
        QDesktopServices::unsetUrlHandler("file");
    }
};
QTEST_MAIN(GuiTest)
#include "gui-test.moc"
