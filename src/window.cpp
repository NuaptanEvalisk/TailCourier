// SPDX-FileCopyrightText: 2026 TailCourier contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "window.h"
#include "storage.h"
#include <KNotification>
#include <KStatusNotifierItem>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <unistd.h>
Window::Window() {
    setWindowTitle("TailCourier");
    setWindowIcon(QIcon::fromTheme("mail-send"));
    resize(680, 480);
    setAcceptDrops(true);
    auto root = new QWidget(this);
    auto layout = new QVBoxLayout(root);
    setCentralWidget(root);
    auto title = new QLabel("<b>TailCourier</b> · Share files between your devices");
    layout->addWidget(title);
    state = new QLabel("Checking Tailscale…");
    state->setTextFormat(Qt::PlainText);
    state->setWordWrap(true);
    state->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(state);
    auto recovery = new QLabel;
    recovery->setTextFormat(Qt::PlainText);
    recovery->setWordWrap(true);
    recovery->hide();
    layout->addWidget(recovery);
    connect(&backend, &Backend::recoveryStatus, this, [recovery](const QString &text) {
        recovery->setText(text);
        recovery->setVisible(!text.isEmpty());
    });
    tabs = new QTabWidget;
    layout->addWidget(tabs);
    auto sendPage = new QWidget;
    auto sendLayout = new QVBoxLayout(sendPage);
    devices = new QTreeWidget;
    devices->setHeaderLabels({"Device", "OS", "Status"});
    devices->setRootIsDecorated(false);
    devices->setSelectionMode(QAbstractItemView::SingleSelection);
    devices->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    devices->header()->setStretchLastSection(true);
    sendLayout->addWidget(devices);
    selection = new QLabel("Drop files here or choose files. Folders are not supported yet.");
    selection->setTextFormat(Qt::PlainText);
    selection->setWordWrap(true);
    sendLayout->addWidget(selection);
    auto row = new QHBoxLayout;
    auto pick = new QPushButton(QIcon::fromTheme("document-open"), "Choose Files…");
    auto send = new QPushButton(QIcon::fromTheme("document-send"), "Send to Selected Device");
    auto refresh = new QPushButton(QIcon::fromTheme("view-refresh"), "Refresh");
    row->addWidget(pick);
    row->addWidget(send);
    row->addStretch();
    row->addWidget(refresh);
    sendLayout->addLayout(row);
    tabs->addTab(sendPage, QIcon::fromTheme("network-workgroup"), "Devices");
    auto historyPage = new QWidget;
    auto hl = new QVBoxLayout(historyPage);
    records = new QTreeWidget;
    records->setHeaderLabels({"Time", "Direction", "File", "Device", "Result"});
    records->setRootIsDecorated(false);
    records->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    hl->addWidget(records);
    auto hr = new QHBoxLayout;
    auto openFolder = new QPushButton("Open Containing Folder");
    auto clear = new QPushButton("Clear History");
    hr->addWidget(openFolder);
    hr->addStretch();
    hr->addWidget(clear);
    hl->addLayout(hr);
    tabs->addTab(historyPage, QIcon::fromTheme("view-history"), "Transfer History");
    progressText = new QLabel;
    progressText->setTextFormat(Qt::PlainText);
    progressText->setWordWrap(true);
    progress = new QProgressBar;
    progress->setTextVisible(false);
    progress->hide();
    layout->addWidget(progressText);
    layout->addWidget(progress);
    auto bottom = new QHBoxLayout;
    auto config = new QPushButton(QIcon::fromTheme("configure"), "Settings…");
    auto retry = new QPushButton(QIcon::fromTheme("view-refresh"), "Retry");
    auto cancel = new QPushButton("Cancel Sending");
    bottom->addWidget(config);
    bottom->addStretch();
    bottom->addWidget(retry);
    bottom->addWidget(cancel);
    layout->addLayout(bottom);
    auto tray = new KStatusNotifierItem("tailcourier", this);
    tray->setTitle("TailCourier");
    tray->setIconByName("mail-send");
    tray->setCategory(KStatusNotifierItem::Communications);
    tray->setStatus(KStatusNotifierItem::Active);
    tray->setToolTip("mail-send", "TailCourier", "Taildrop file transfer");
    winId();
    tray->setAssociatedWindow(windowHandle());
    connect(tray, &KStatusNotifierItem::activateRequested, this, [this] {
        show();
        raise();
    });
    auto menu = tray->contextMenu();
    menu->addAction("Devices", this, [this] {
        tabs->setCurrentIndex(0);
        show();
        raise();
    });
    menu->addAction("Send Files…", this, &Window::chooseFiles);
    menu->addAction("Transfer History", this, [this] {
        tabs->setCurrentIndex(1);
        show();
        raise();
    });
    menu->addAction("Settings…", this, &Window::settings);
    pause = menu->addAction("Pause Automatic Receiving");
    pause->setCheckable(true);
    pause->setChecked(backend.paused);
    connect(pause, &QAction::toggled, &backend, &Backend::setPaused);
    tray->setStandardActionsEnabled(false);
    menu->addSeparator();
    menu->addAction("Quit", this, &Window::quit);
    connect(pick, &QPushButton::clicked, this, &Window::chooseFiles);
    connect(send, &QPushButton::clicked, this, &Window::sendSelected);
    connect(refresh, &QPushButton::clicked, &backend, &Backend::refresh);
    connect(config, &QPushButton::clicked, this, &Window::settings);
    connect(retry, &QPushButton::clicked, &backend, &Backend::retry);
    connect(cancel, &QPushButton::clicked, &backend, &Backend::cancelSend);
    connect(clear, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "Clear History", "Clear local transfer history? Your files will be kept.") ==
            QMessageBox::Yes)
            backend.clearHistory();
    });
    connect(openFolder, &QPushButton::clicked, this, [this] {
        if (auto item = records->currentItem())
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(QFileInfo(item->data(0, Qt::UserRole).toString()).absolutePath()));
    });
    connect(&backend, &Backend::status, state, &QLabel::setText);
    connect(&backend, &Backend::status, tray, [tray](const QString &text) {
        tray->setToolTip("mail-send", "TailCourier", text);
    });
    connect(&backend, &Backend::devicesChanged, this, [this] {
        QString selected;
        if (auto i = devices->currentItem())
            selected = i->data(0, Qt::UserRole).toString();
        devices->clear();
        for (const auto &d : backend.devices) {
            auto i = new QTreeWidgetItem(devices, {d.name, d.os, d.reason});
            i->setData(0, Qt::UserRole, d.ip);
            i->setIcon(0, QIcon::fromTheme(d.os == "iOS" || d.os == "android" ? "smartphone" : "computer"));
            if (!d.eligible)
                i->setForeground(2, palette().brush(QPalette::Disabled, QPalette::Text));
            if (d.ip == selected)
                devices->setCurrentItem(i);
        }
    });
    connect(&backend, &Backend::sendStatus, this, [this](const QString &s, bool active) {
        progressText->setText(s);
        progress->setRange(0, 0);
        progress->setVisible(active);
    });
    connect(&backend, &Backend::sendFailed, this, [this](const QString &s) {
        progressText->setText("Sending failed: " + s + "\nChoose Retry to resend failed files.");
    });
    connect(&backend, &Backend::historyChanged, this, &Window::showHistory);
    showHistory();
    notificationTimer.setSingleShot(true);
    notificationTimer.setInterval(4000);
    connect(&notificationTimer, &QTimer::timeout, this, &Window::notifyReceived);
    connect(&backend, &Backend::received, this, [this](const QStringList &p) {
        notificationFiles += p;
        if (!notificationTimer.isActive())
            notificationTimer.start();
    });
    QTimer::singleShot(0, &backend, &Backend::start);
}
void Window::chooseFiles() {
    show();
    raise();
    auto p = QFileDialog::getOpenFileNames(this, "Choose Files to Send");
    if (!p.isEmpty())
        selectFiles(p);
}
void Window::selectFiles(QStringList paths) {
    for (const auto &p : paths) {
        QFileInfo f(p);
        if (!f.isFile() || f.isSymLink()) {
            QMessageBox::warning(this, "Unsupported File",
                                 "Choose one or more regular files. Folders and symbolic links are not supported.\n" + p);
            return;
        }
    }
    paths.removeDuplicates();
    files = paths;
    selection->setText(files.size() == 1 ? "1 file selected · Choose a device, then send"
                                       : QString("%1 files selected · Choose a device, then send").arg(files.size()));
    selection->setToolTip(files.join('\n'));
    tabs->setCurrentIndex(0);
    show();
    raise();
}
void Window::sendSelected() {
    auto item = devices->currentItem();
    if (!item || files.isEmpty()) {
        progressText->setText("Choose files and a destination device first.");
        return;
    }
    for (const auto &d : backend.devices)
        if (d.ip == item->data(0, Qt::UserRole).toString()) {
            backend.send(files, d);
            return;
        }
}
void Window::showHistory() {
    records->clear();
    for (const auto &v : backend.history) {
        auto o = v.toObject();
        auto p = o["path"].toString();
        auto i = new QTreeWidgetItem(
            records,
            {QDateTime::fromString(o["time"].toString(), Qt::ISODate).toLocalTime().toString("MM-dd HH:mm"),
             o["direction"].toString(), QFileInfo(p).fileName(), o["peer"].toString(),
             o["result"].toString()});
        i->setData(0, Qt::UserRole, p);
        i->setToolTip(2, p);
    }
}
void Window::notifyReceived() {
    auto paths = notificationFiles;
    notificationFiles.clear();
    if (paths.isEmpty())
        return;
    auto n = new KNotification("received", KNotification::CloseOnTimeout, this);
    n->setComponentName("tailcourier");
    n->setTitle(paths.size() == 1 ? "File received" : QString("%1 files received").arg(paths.size()));
    n->setText(paths.size() == 1 ? QFileInfo(paths.first()).fileName() : "Files saved. See Transfer History for details.");
    n->setIconName("mail-attachment");
    if (paths.size() == 1) {
        auto a = n->addAction("Open File");
        connect(a, &KNotificationAction::activated, this, [this, p = paths.first()] {
            if (QMessageBox::question(this, "Open Received File",
                                      "Open this file only if you trust its sender and contents.\n" + QFileInfo(p).fileName()) ==
                QMessageBox::Yes)
                QDesktopServices::openUrl(QUrl::fromLocalFile(p));
        });
    }
    auto folder = n->addAction("Open Containing Folder");
    connect(folder, &KNotificationAction::activated, this, [p = paths.first()] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(p).absolutePath()));
    });
    n->sendEvent();
}
void Window::settings() {
    QDialog d(this);
    d.setWindowTitle("TailCourier Settings");
    auto l = new QVBoxLayout(&d);
    auto form = new QFormLayout;
    auto path = new QLineEdit(backend.destination);
    auto browse = new QPushButton("Browse…");
    auto r = new QHBoxLayout;
    r->addWidget(path);
    r->addWidget(browse);
    form->addRow("Download folder", r);
    l->addLayout(form);
    auto autoStart = new QCheckBox("Start TailCourier when I log in to Plasma");
    const auto autoPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
                          "/autostart/org.kde.tailcourier.desktop";
    autoStart->setChecked(QFile::exists(autoPath));
    l->addWidget(autoStart);
    auto pauseBox = new QCheckBox("Pause automatic receiving");
    pauseBox->setChecked(backend.paused);
    l->addWidget(pauseBox);
    auto info = new QLabel("Pausing or quitting stops inbox retrieval. Tailscale may still receive files into its inbox.\n"
                           "Pausing does not stop sending; files already retrieved may finish saving. Quitting cancels sending, "
                           "but the other device may keep partial or complete files.\n"
                           "Interrupted downloads remain in staging and may be incomplete.");
    info->setWordWrap(true);
    l->addWidget(info);
    auto staging = new QPushButton("Open Staging Folder…");
    l->addWidget(staging);
    connect(staging, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(backend.stagingRoot)); });
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    l->addWidget(buttons);
    connect(browse, &QPushButton::clicked, &d, [&] {
        auto p = QFileDialog::getExistingDirectory(&d, "Download Folder", path->text());
        if (!p.isEmpty())
            path->setText(p);
    });
    connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &d, [&] {
        QString err;
        int fd = Storage::directory(path->text(), err);
        if (fd < 0) {
            QMessageBox::warning(&d, "Folder Unavailable", err);
            return;
        }
        ::close(fd);
        if (autoStart->isChecked()) {
            QDir().mkpath(QFileInfo(autoPath).absolutePath());
            QSaveFile f(autoPath);
            QString exe = QCoreApplication::applicationFilePath();
            exe.replace('\\', "\\\\")
                .replace('"', "\\\"")
                .replace('`', "\\`")
                .replace('$', "\\$")
                .replace('%', "%%");
            if (!f.open(QIODevice::WriteOnly)) {
                QMessageBox::warning(&d, "Unable to Save", f.errorString());
                return;
            }
            f.write(("[Desktop Entry]\nType=Application\nName=TailCourier\nIcon=mail-send\nExec=\"" + exe +
                     "\" --background\nOnlyShowIn=KDE;\n")
                        .toUtf8());
            if (!f.commit()) {
                QMessageBox::warning(&d, "Unable to Save", f.errorString());
                return;
            }
        } else if (QFile::exists(autoPath) && !QFile::remove(autoPath)) {
            QMessageBox::warning(&d, "Unable to Disable Autostart", "Could not remove the autostart entry.");
            return;
        }
        backend.destination = path->text();
        QSettings().setValue("destination", backend.destination);
        pause->setChecked(pauseBox->isChecked());
        d.accept();
    });
    d.exec();
}
void Window::closeEvent(QCloseEvent *e) {
    hide();
    e->ignore();
}
void Window::quit() {
    if (backend.sending &&
        QMessageBox::question(this, "Quit TailCourier",
                              "Quit and cancel sending? The other device may already have partial or complete files.") != QMessageBox::Yes)
        return;
    backend.stop();
    qApp->quit();
}
void Window::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}
void Window::dropEvent(QDropEvent *e) {
    QStringList p;
    for (const auto &u : e->mimeData()->urls()) {
        if (!u.isLocalFile()) {
            QMessageBox::warning(this, "Unsupported Location", "Only local files are supported.");
            return;
        }
        p << u.toLocalFile();
    }
    selectFiles(p);
    e->acceptProposedAction();
}
void Window::acceptArguments(const QStringList &args, const QString &cwd) {
    QStringList p;
    bool positional = false;
    for (int i = 1; i < args.size(); ++i) {
        const auto a = args[i];
        if (!positional && a == "--") {
            positional = true;
            continue;
        }
        if (!positional && (a == "--send" || a == "--background"))
            continue;
        auto u = QUrl(a);
        if (u.isLocalFile())
            p << u.toLocalFile();
        else if (u.scheme().isEmpty())
            p << QDir(cwd).absoluteFilePath(a);
        else {
            progressText->setText("Only local files are supported.");
            return;
        }
    }
    if (!p.isEmpty())
        selectFiles(p);
    else if (!args.contains("--background")) {
        show();
        raise();
    }
}
