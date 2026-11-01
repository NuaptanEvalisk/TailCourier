// SPDX-FileCopyrightText: 2026 Nuaptan Evalisk
// SPDX-License-Identifier: GPL-3.0-only

#include <KFileItem>
#include <KFileItemActions>
#include <KFileItemListProperties>
#include <QAction>
#include <QFile>
#include <QMenu>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

class DolphinTest : public QObject {
    Q_OBJECT
private slots:
    void installedMenu() {
        const auto entry = QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                                  "kio/servicemenus/tailcourier-send.desktop");
        QVERIFY2(!entry.isEmpty(), "TailCourier service menu was not discovered");
        QVERIFY(QFileInfo(entry).isExecutable());
        QTemporaryDir temporary;
        const QStringList paths = {temporary.filePath("first file.txt"),
                                   temporary.filePath("-中文 'second'.txt")};
        KFileItemList files;
        for (const auto &path : paths) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("Non-sensitive menu test fixture\n");
            f.close();
            files.append(KFileItem(QUrl::fromLocalFile(path)));
        }
        auto countActions = [](const KFileItemList &items) {
            KFileItemActions actions;
            actions.setItemListProperties(KFileItemListProperties(items));
            QMenu menu;
            actions.addActionsTo(&menu, KFileItemActions::MenuActionSource::Services);
            int count = 0;
            for (auto action : menu.actions())
                count += action->text() == "Send with TailCourier";
            return count;
        };
        QCOMPARE(countActions({files.first()}), 1);
        QCOMPARE(countActions(files), 1);
        QCOMPARE(countActions({KFileItem(QUrl::fromLocalFile(temporary.path()))}), 0);
        qInfo().noquote() << "Verified top-level menu for single and multiple files:" << entry;
        // Never trigger an action here: this test checks discovery, not real file sending.
    }
};

QTEST_MAIN(DolphinTest)
#include "dolphin-test.moc"
