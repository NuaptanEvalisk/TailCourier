// SPDX-FileCopyrightText: 2026 Nuaptan Evalisk
// SPDX-License-Identifier: GPL-3.0-only

#include "backend.h"
#include "storage.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <unistd.h>
class CoreTest : public QObject {
    Q_OBJECT
    QString cli;
    QTemporaryDir temp;
    void put(QString path, QByteArray data = "hello") {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(data), data.size());
    }
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName("tailcourier-test-" + QString::number(getpid()));
        QSettings().clear();
        cli = temp.path() + "/tailscale";
        put(cli, R"PY(#!/usr/bin/python3
import sys,os,time,json
args=sys.argv[1:]
if args == ['version']: print('1.102.4'); sys.exit(0)
if '--help' in args: print('--targets --wait --conflict --update-interval'); sys.exit(0)
if args == ['status','--json']:
 print(json.dumps({'BackendState':'Running','Peer':{}})); sys.exit(0)
if args[:2] == ['file','cp']:
 with open(os.environ['TC_LOG'],'a') as f: f.write(json.dumps(args)+'\n')
 if os.environ.get('TC_FAIL')=='1': print('403 permission denied',file=sys.stderr);sys.exit(1)
 if os.environ.get('TC_SLOW')=='1': time.sleep(60)
 sys.exit(0)
if args[:2] == ['file','get']:
 if os.environ.get('TC_GET_FAIL')=='1': print('403 permission denied',file=sys.stderr);sys.exit(1)
 fixture=os.environ.get('TC_RECEIVE','')
 if fixture and os.path.exists(fixture):
  os.unlink(fixture)
  open(os.path.join(args[-1],'收到的文件.txt'),'w').write('safe payload')
  open(os.path.join(args[-1],'.complete'),'w').write('ordinary filename')
  sys.exit(0)
 time.sleep(60)
)PY");
        QFile::setPermissions(cli, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    void names() {
        QVERIFY(Storage::validName("-中文 '引号'.txt"));
        for (auto n : {"..", "../x", "a/b", "a\\b", "x\ny", ""})
            QVERIFY(!Storage::validName(n));
    }
    void collisionAndSymlink() {
        QTemporaryDir d;
        QDir().mkdir(d.path() + "/dest");
        put(d.path() + "/x.txt");
        put(d.path() + "/dest/x.txt", "existing");
        QString e;
        auto p = Storage::publish(d.path() + "/x.txt", d.path() + "/dest", e);
        QCOMPARE(QFileInfo(p).fileName(), "x (1).txt");
        QFile old(d.path() + "/dest/x.txt");
        QVERIFY(old.open(QIODevice::ReadOnly));
        QCOMPARE(old.readAll(), "existing");
        put(d.path() + "/y.txt");
        QVERIFY(QFile::link(d.path() + "/dest", d.path() + "/link"));
        QVERIFY(Storage::publish(d.path() + "/y.txt", d.path() + "/link", e).isEmpty());
        QVERIFY(QFile::exists(d.path() + "/y.txt"));
        QVERIFY(QFile::link(d.path() + "/y.txt", d.path() + "/sym.txt"));
        QVERIFY(Storage::publish(d.path() + "/sym.txt", d.path() + "/dest", e).isEmpty());
        QVERIFY(QFile::link(d.path() + "/y.txt", d.path() + "/dest/y.txt"));
        QVERIFY(!Storage::publish(d.path() + "/y.txt", d.path() + "/dest", e).isEmpty());
    }
    void deviceStates() {
        QString err;
        auto data =
            R"({"BackendState":"Running","Peer":{"a":{"HostName":"phone","OS":"iOS","Online":true,"TaildropTarget":1,"TailscaleIPs":["100.1.2.3"]},"b":{"HostName":"offline","Online":false,"TaildropTarget":5},"c":{"HostName":"other","Online":true,"TaildropTarget":9}}})";
        auto list = parseDevices(data, err);
        QCOMPARE(list.size(), 3);
        int eligible = 0;
        for (const auto &d : list)
            eligible += d.eligible;
        QCOMPARE(eligible, 1);
        QVERIFY(parseDevices(R"({"BackendState":"NeedsLogin"})", err).isEmpty());
        QVERIFY(err.contains("Not signed in"));
        QVERIFY(parseDevices("bad", err).isEmpty());
    }
    void multipleAndRetry() {
        auto log = temp.path() + "/args.jsonl";
        qputenv("TC_LOG", log.toUtf8());
        qputenv("TC_FAIL", "1");
        QString a = temp.path() + "/-中文 ' space.txt", b = temp.path() + "/second.txt";
        put(a);
        put(b);
        Backend backend(nullptr, cli);
        Device d{"phone", "iOS", "100.1.2.3", "", true, true};
        backend.send({a, b}, d);
        QTRY_VERIFY(!backend.sending);
        QCOMPARE(backend.history.size(), 2);
        qputenv("TC_FAIL", "0");
        backend.retry();
        QTRY_VERIFY(!backend.sending);
        QCOMPARE(backend.history.size(), 4);
        QFile f(log);
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto lines = f.readAll().trimmed().split('\n');
        QCOMPARE(lines.size(), 4);
        auto args = QJsonDocument::fromJson(lines.first()).array();
        QCOMPARE(args[3].toString(), a);
        QCOMPARE(args[4].toString(), "100.1.2.3:");
        d.eligible = false;
        QSignalSpy error(&backend, &Backend::sendFailed);
        backend.send({a}, d);
        QCOMPARE(error.size(), 1);
    }
    void cancel() {
        qputenv("TC_SLOW", "1");
        Backend b(nullptr, cli);
        auto p = temp.path() + "/cancel.txt";
        put(p);
        b.send({p}, Device{"phone", "iOS", "100.1.2.3", "", true, true});
        QTest::qWait(100);
        b.cancelSend();
        QTRY_VERIFY(!b.sending);
        QVERIFY(b.history.first().toObject()["result"].toString().contains("Cancelled"));
        qunsetenv("TC_SLOW");
    }
    void receiveAndRestart() {
        QString dest = temp.path() + "/downloads", fixture = temp.path() + "/receive-trigger";
        put(fixture);
        qputenv("TC_RECEIVE", fixture.toUtf8());
        {
            Backend b(nullptr, cli);
            b.destination = dest;
            QSignalSpy spy(&b, &Backend::received);
            b.start();
            QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
            QVERIFY(QFile::exists(dest + "/收到的文件.txt"));
            QVERIFY(QFile::exists(dest + "/.complete"));
            b.setPaused(true);
            b.stop();
        }
        {
            Backend b(nullptr, cli);
            QVERIFY(!b.history.isEmpty());
            b.clearHistory();
        }
        {
            Backend b(nullptr, cli);
            QVERIFY(b.history.isEmpty());
        }
    }
    void unwritable() {
        QTemporaryDir d;
        put(d.path() + "/x");
        QString e;
        QVERIFY(Storage::publish(d.path() + "/x", "/proc/tailcourier-test", e).isEmpty());
        QVERIFY(QFile::exists(d.path() + "/x"));
    }
    void interruptedRecovery() {
        QSettings().setValue("paused", false);
        Backend b(nullptr, cli);
        b.destination = temp.path() + "/recovered";
        const auto complete = b.stagingRoot + "/complete-batch";
        const auto partial = b.stagingRoot + "/partial-batch";
        QVERIFY(QDir().mkpath(complete));
        QVERIFY(QDir().mkpath(partial));
        put(complete + "/ready.txt", "complete payload");
        put(complete + ".complete", "");
        put(partial + "/partial.txt", "incomplete payload");
        QSignalSpy recovery(&b, &Backend::recoveryStatus);
        QSignalSpy received(&b, &Backend::received);
        b.start();
        QTRY_COMPARE_WITH_TIMEOUT(received.size(), 1, 5000);
        QVERIFY(QFile::exists(b.destination + "/ready.txt"));
        QVERIFY(!QFile::exists(b.destination + "/partial.txt"));
        QVERIFY(QFile::exists(partial + "/partial.txt"));
        QVERIFY(!recovery.isEmpty());
        QVERIFY(recovery.first().first().toString().contains("interrupted batch"));
    }
    void receivePermissionError() {
        QSettings().setValue("paused", false);
        qputenv("TC_GET_FAIL", "1");
        Backend b(nullptr, cli);
        b.destination = temp.path() + "/permission-test";
        QSignalSpy status(&b, &Backend::status);
        b.start();
        auto denied = [&] {
            for (const auto &event : status)
                if (event.first().toString().contains("Permission denied")) return true;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(denied(), 5000);
        b.stop();
        qunsetenv("TC_GET_FAIL");
    }
    void cleanupTestCase() {
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).removeRecursively();
        QSettings().clear();
    }
};
QTEST_GUILESS_MAIN(CoreTest)
#include "core-test.moc"
