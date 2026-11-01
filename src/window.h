// SPDX-FileCopyrightText: 2026 TailCourier contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "backend.h"
#include <QMainWindow>
class QTreeWidget;
class QLabel;
class QProgressBar;
class QTabWidget;
class QAction;
class Window : public QMainWindow {
    Q_OBJECT
  public:
    explicit Window();
    void acceptArguments(const QStringList &args, const QString &cwd);
    Backend backend;

  protected:
    void closeEvent(QCloseEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

  private:
    QTreeWidget *devices, *records;
    QLabel *state, *selection, *progressText;
    QProgressBar *progress;
    QTabWidget *tabs;
    QAction *pause;
    QStringList files, notificationFiles;
    QTimer notificationTimer;
    void chooseFiles();
    void selectFiles(QStringList);
    void sendSelected();
    void settings();
    void showHistory();
    void notifyReceived();
    void quit();
};
