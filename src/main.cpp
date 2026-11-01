// SPDX-FileCopyrightText: 2026 TailCourier contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "window.h"
#include <KDBusService>
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QLockFile>
#include <QMessageBox>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
static int signalFd = -1;
static void terminateSignal(int) {
    const char byte = 1;
    if (signalFd >= 0) {
        auto ignored = ::write(signalFd, &byte, 1);
        (void)ignored;
    }
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("tailcourier");
    app.setOrganizationName("TailCourier");
    app.setApplicationVersion("0.1.0");
    app.setDesktopFileName("org.kde.tailcourier");
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription("A native Taildrop file transfer client for Plasma 6");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"send", "Select files and open the device picker"});
    parser.addOption({"background", "Start in the system tray"});
    parser.addPositionalArgument("files", "Local files to send", "[files...]");
    parser.process(app);
    if (getuid() == 0) {
        QMessageBox::critical(nullptr, "TailCourier",
                              "Run TailCourier as a regular user. The GUI must not run as root.");
        return 1;
    }
    KDBusService service(KDBusService::Unique);
    QLockFile lock(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/tailcourier.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) {
        QMessageBox::warning(nullptr, "TailCourier", "Another instance is running, or the instance lock could not be acquired.");
        return 1;
    }
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) != 0)
        return 1;
    signalFd = sockets[0];
    QSocketNotifier notifier(sockets[1], QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&] {
        notifier.setEnabled(false);
        app.quit();
    });
    ::signal(SIGTERM, terminateSignal);
    ::signal(SIGINT, terminateSignal);
    Window window;
    QObject::connect(&service, &KDBusService::activateRequested, &window, &Window::acceptArguments);
    window.acceptArguments(app.arguments(), QDir::currentPath());
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &window.backend, &Backend::stop);
    return app.exec();
}
