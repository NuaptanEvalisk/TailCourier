// SPDX-FileCopyrightText: 2026 TailCourier contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "backend.h"
#include "storage.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

QList<Device> parseDevices(const QByteArray &data, QString &error) {
    QJsonParseError e;
    auto doc = QJsonDocument::fromJson(data, &e);
    if (e.error != QJsonParseError::NoError || !doc.isObject()) {
        error = "Invalid Tailscale status output";
        return {};
    }
    auto root = doc.object();
    auto state = root["BackendState"].toString();
    if (state != "Running") {
        error = state == "NeedsLogin" ? "Not signed in. Sign in using Tailscale."
                                      : "Tailscale is not running: " + state;
        return {};
    }
    QList<Device> result;
    const auto peers = root["Peer"].toObject();
    for (auto it = peers.begin(); it != peers.end(); ++it) {
        auto p = it.value().toObject();
        Device d;
        d.name = p["HostName"].toString();
        d.os = p["OS"].toString();
        d.ip = p["TailscaleIPs"].toArray().first().toString();
        d.online = p["Online"].toBool();
        auto eligibility = p["TaildropTarget"].toInt(0);
        d.eligible = eligibility == 1 && d.online && !d.ip.isEmpty();
        switch (eligibility) {
        case 1:
            d.reason = d.online ? "Ready to receive" : "Offline";
            break;
        case 2:
            d.reason = "Network map not ready";
            break;
        case 3:
            d.reason = "Tailscale is not running";
            break;
        case 4:
            d.reason = "Taildrop disabled or blocked by policy";
            break;
        case 5:
            d.reason = "Offline";
            break;
        case 6:
            d.reason = "Device information unavailable";
            break;
        case 7:
            d.reason = "Device OS does not support Taildrop";
            break;
        case 8:
            d.reason = "Device does not provide a Taildrop endpoint";
            break;
        case 9:
            d.reason = "Owned by another user; not eligible for Taildrop";
            break;
        default:
            d.reason = "Unknown Taildrop eligibility; update Tailscale";
        }
        if (!p["NoFileSharingReason"].toString().isEmpty())
            d.reason += " · " + p["NoFileSharingReason"].toString();
        result.append(d);
    }
    std::sort(result.begin(), result.end(),
              [](const Device &a, const Device &b) { return a.name.localeAwareCompare(b.name) < 0; });
    return result;
}
static QString explain(QString e) {
    e = e.trimmed().right(1800);
    if (e.contains("denied", Qt::CaseInsensitive) || e.contains("permission", Qt::CaseInsensitive) ||
        e.contains("403"))
        return "Permission denied. TailCourier will not elevate privileges or change VPN settings. See Permissions in the README.\n" + e;
    if (e.contains("connect", Qt::CaseInsensitive) || e.contains("socket", Qt::CaseInsensitive))
        return "Cannot connect to Tailscale. Check that tailscaled is running.\n" + e;
    return e;
}
Backend::Backend(QObject *parent, QString cli)
    : QObject(parent), executable(cli.isEmpty() ? QStandardPaths::findExecutable("tailscale") : cli) {
    for (auto process : {&receiver, &sender, &query}) {
        process->setChildProcessModifier([] {
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() == 1)
                _exit(1);
        });
    }
    QSettings s;
    destination = s.value("destination",
                          QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/TailCourier")
                      .toString();
    paused = s.value("paused", false).toBool();
    stagingRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/inbox";
    int stagingFd = Storage::directory(stagingRoot, stagingError);
    if (stagingFd >= 0) {
        struct stat st{};
        if (::fstat(stagingFd, &st) != 0 || st.st_uid != ::getuid() || ::fchmod(stagingFd, 0700) != 0)
            stagingError = "The staging folder must be owned by the current user and support private permissions";
        ::close(stagingFd);
    }
    QFile h(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/history.json");
    if (h.open(QIODevice::ReadOnly))
        history = QJsonDocument::fromJson(h.readAll()).array();
    while (history.size() > 500)
        history.removeLast();
    retryTimer.setSingleShot(true);
    connect(&retryTimer, &QTimer::timeout, this, &Backend::receive);
    refreshTimer.setInterval(60000);
    connect(&refreshTimer, &QTimer::timeout, this, &Backend::refresh);
    connect(&receiver, &QProcess::finished, this,
            [this](int c, QProcess::ExitStatus s) { finishReceive(s == QProcess::NormalExit ? c : -1); });
    connect(&receiver, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finishReceive(-1);
    });
    connect(&sender, &QProcess::finished, this, [this](int c, QProcess::ExitStatus s) {
        QString path = pending.takeFirst();
        bool ok = c == 0 && s == QProcess::NormalExit && !cancelled;
        QString detail = ok          ? "Sent"
                         : cancelled ? "Cancelled (the other device may have partial or complete files)"
                                     : explain(QString::fromUtf8(sender.readAllStandardError()));
        if (!ok && detail.isEmpty())
            detail = QString("Tailscale send process exited unexpectedly (%1)").arg(c);
        record("Sent", path, targetName, detail);
        if (!ok)
            failed.append(path);
        ++completed;
        if (!ok && !cancelled) {
            lastSendError = detail;
            emit sendFailed(detail);
        }
        if (cancelled) {
            failed += pending;
            pending.clear();
        }
        nextSend();
    });
    connect(&sender, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            failed += pending;
            pending.clear();
            sending = false;
            emit sendStatus("Unable to start the Tailscale CLI", false);
        }
    });
    // Progress output is not parsed as percentages; discard it continuously.
    connect(&sender, &QProcess::readyReadStandardOutput, this, [this] { sender.readAllStandardOutput(); });
    connect(&query, &QProcess::finished, this, [this](int c, QProcess::ExitStatus s) {
        QString error;
        if (c == 0 && s == QProcess::NormalExit)
            devices = parseDevices(query.readAllStandardOutput(), error);
        else {
            devices.clear();
            error = explain(QString::fromUtf8(query.readAllStandardError()));
            if (error.isEmpty())
                error = "Unable to read Tailscale status: command timed out or exited unexpectedly";
        }
        emit devicesChanged();
        emit status(error.isEmpty() ? (paused ? "Automatic receiving paused" : "Tailscale connected · Waiting for files") : error);
    });
    connect(&query, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            emit status("Unable to start the Tailscale CLI");
    });
}
Backend::~Backend() {
    stop();
}
void Backend::start() {
    if (!stagingError.isEmpty()) {
        emit status("Staging folder unavailable: " + stagingError);
        return;
    }
    scanRecovery();
    runProbe();
}
void Backend::runProbe(int step) {
    if (executable.isEmpty()) {
        emit status("Tailscale CLI not found. Install Tailscale first.");
        return;
    }
    auto p = new QProcess(this);
    const QList<QStringList> args = {{"version"}, {"file", "cp", "--help"}, {"file", "get", "--help"}};
    connect(p, &QProcess::finished, this, [this, p, step](int c, QProcess::ExitStatus) {
        auto out = p->readAllStandardOutput() + p->readAllStandardError();
        p->deleteLater();
        bool valid = c == 0 &&
                     (step == 0 || (step == 1 ? out.contains("--targets") && out.contains("--update-interval")
                                              : out.contains("--wait") && out.contains("--conflict")));
        if (!valid) {
            emit status("Incompatible Tailscale CLI: file cp, get --wait and --conflict are required. Update Tailscale.");
            return;
        }
        if (step < 2) {
            runProbe(step + 1);
            return;
        }
        supported = true;
        refresh();
        refreshTimer.start();
        receive();
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            emit status("Unable to start the Tailscale CLI");
            p->deleteLater();
        }
    });
    p->start(executable, args[step]);
    QTimer::singleShot(10000, p, [p] {
        if (p->state() != QProcess::NotRunning)
            p->kill();
    });
}
void Backend::refresh() {
    if (query.state() != QProcess::NotRunning)
        return;
    query.start(executable, {"status", "--json"});
    auto timer = new QTimer(&query);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, &query, &QProcess::kill);
    connect(&query, &QProcess::finished, timer, &QObject::deleteLater);
    timer->start(10000);
}
void Backend::setPaused(bool value) {
    paused = value;
    QSettings().setValue("paused", paused);
    if (paused) {
        retryTimer.stop();
        receiver.kill();
        emit status("Inbox retrieval paused. Tailscale may still receive files into its inbox. Sending is unaffected.");
    } else
        receive();
}
void Backend::receive() {
    if (!supported || paused || quitting || receiver.state() != QProcess::NotRunning)
        return;
    if (!publishing.isEmpty()) {
        retryTimer.start(1500);
        return;
    }
    QString error;
    int staging = Storage::directory(stagingRoot, error);
    if (staging < 0) {
        emit status("Staging folder unavailable: " + error);
        return;
    }
    ::close(staging);
    int fd = Storage::directory(destination, error);
    if (fd < 0) {
        emit status("Download folder unavailable: " + error);
        retryTimer.start(60000);
        return;
    }
    ::close(fd);
    scanRecovery();
    // Completed batches are recoverable across restart; interrupted batches are never presented as complete.
    for (const auto &entry : QDir(stagingRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        batch = stagingRoot + "/" + entry;
        if (QFile::exists(batch + ".complete"))
            publishBatch();
    }
    if (!publishing.isEmpty()) {
        retryTimer.start(1500);
        return;
    }
    batch = stagingRoot + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(batch);
    QFile::setPermissions(batch, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    receiver.start(executable, {"file", "get", "--wait", "--conflict=rename", batch});
}
void Backend::finishReceive(int code) {
    if (batch.isEmpty())
        return;
    if (code == 0) {
        QFile marker(batch + ".complete");
        if (marker.open(QIODevice::WriteOnly)) {
            marker.close();
            publishBatch();
        } else {
            emit status("Unable to mark this download complete. Files remain in staging: " + marker.errorString());
        }
        delay = 5000;
    } else if (!paused && !quitting) {
        emit status("Receiving unavailable; retrying with backoff: " +
                    explain(QString::fromUtf8(receiver.readAllStandardError())));
        delay = qMin(delay * 2, 300000);
    }
    QDir().rmdir(batch);
    batch.clear();
    scanRecovery();
    if (!paused && !quitting)
        retryTimer.start(code == 0 ? 1500 : delay);
}
void Backend::publishBatch() {
    // Large local copies run in workers; each batch has at most one publication worker.
    if (!publishing.isEmpty())
        return;
    publishing.insert(batch);
    const QString current = batch, output = destination;
    // A nested event loop is deliberately avoided: worker completion returns through Qt signals.
    auto worker = QThread::create([this, current, output] {
        QStringList done;
        QString err;
        for (const auto &f : QDir(current).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot)) {
            auto p = Storage::publish(current + "/" + f, output, err, &stopPublish);
            if (!p.isEmpty())
                done << p;
        }
        QMetaObject::invokeMethod(
            this,
            [this, current, done, err] {
                publishing.remove(current);
                for (const auto &p : done)
                    record("Received", p, {}, "Received");
                if (!done.isEmpty())
                    emit received(done);
                if (!err.isEmpty()) {
                    emit status("Unable to save received files. Complete copies remain in staging: " + err);
                    delay = qMin(delay * 2, 300000);
                    if (!paused && !quitting)
                        retryTimer.start(delay);
                }
                if (QDir(current).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty()) {
                    QFile::remove(current + ".complete");
                    QDir().rmdir(current);
                }
            },
            Qt::QueuedConnection);
    });
    worker->setParent(this);
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}
void Backend::send(const QStringList &paths, const Device &device) {
    if (sending) {
        emit sendFailed("A transfer is already running. Wait for it to finish or cancel it.");
        return;
    }
    if (!device.eligible) {
        emit sendFailed(device.reason);
        return;
    }
    for (const auto &p : paths) {
        QFileInfo f(p);
        if (!f.isFile() || f.isSymLink() || !f.isReadable()) {
            emit sendFailed("Only readable regular files are supported, not folders or symbolic links: " + p);
            return;
        }
    }
    pending.clear();
    for (const auto &p : paths)
        pending << QFileInfo(p).absoluteFilePath();
    if (pending.isEmpty())
        return;
    failed.clear();
    lastSendError.clear();
    target = device.ip;
    targetName = device.name;
    total = pending.size();
    completed = 0;
    cancelled = false;
    sending = true;
    nextSend();
}
void Backend::nextSend() {
    if (quitting) {
        pending.clear();
        sending = false;
        return;
    }
    if (pending.isEmpty()) {
        sending = false;
        emit sendStatus(cancelled          ? "Sending cancelled"
                        : failed.isEmpty() ? "Sending complete"
                                           : "Some files failed. Choose Retry: " + lastSendError,
                        false);
        return;
    }
    emit sendStatus(QString("Sending %1 of %2 · %3")
                        .arg(completed + 1)
                        .arg(total)
                        .arg(QFileInfo(pending.first()).fileName()),
                    true);
    sender.start(executable, {"file", "cp", "--update-interval=0", pending.first(), target + ":"});
}
void Backend::cancelSend() {
    cancelled = true;
    sender.kill();
}
void Backend::retry() {
    if (!sending && !failed.isEmpty()) {
        pending = failed;
        failed.clear();
        total = pending.size();
        completed = 0;
        cancelled = false;
        sending = true;
        nextSend();
    }
    if (!paused) {
        retryTimer.stop();
        receive();
    }
    refresh();
}
void Backend::record(QString direction, QString path, QString peer, QString result) {
    history.prepend(QJsonObject{{"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                                {"direction", direction},
                                {"path", path},
                                {"peer", peer},
                                {"result", result.left(200)}});
    while (history.size() > 500)
        history.removeLast();
    saveHistory();
    emit historyChanged();
}
void Backend::saveHistory() {
    QSaveFile f(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/history.json");
    if (f.open(QIODevice::WriteOnly)) {
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        f.write(QJsonDocument(history).toJson());
        if (!f.commit())
            emit status("Unable to save transfer history: " + f.errorString());
    } else
        emit status("Unable to save transfer history: " + f.errorString());
}
void Backend::clearHistory() {
    history = {};
    saveHistory();
    emit historyChanged();
}
void Backend::stop() {
    quitting = true;
    cancelled = true;
    stopPublish.store(true);
    retryTimer.stop();
    refreshTimer.stop();
    for (auto p : {&receiver, &sender, &query}) {
        p->kill();
        p->waitForFinished(2000);
    }
    for (auto t : findChildren<QThread *>())
        t->wait();
}

void Backend::scanRecovery() {
    int count = 0;
    for (const auto &entry : QDir(stagingRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto path = stagingRoot + "/" + entry;
        if (!QFile::exists(path + ".complete") &&
            !QDir(path).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty())
            ++count;
    }
    emit recoveryStatus(
        count ? QString("%1 interrupted batch(es) remain in staging. Open the staging folder in Settings and verify files with the sender before recovery; they may be incomplete.")
                    .arg(count)
              : QString());
}
