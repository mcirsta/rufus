// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/prepare_source.h"
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <unistd.h>

extern "C" int IsIsoScanCancelled(void) { return 0; }

class TestPrepareSource : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    rufus::PrepareImageRequest request_;

private slots:
    void init()
    {
        QVERIFY(directory_.isValid());
        request_ = {};
        const QString path = directory_.filePath(QStringLiteral("source.iso"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write("source bytes"), 12);
        file.close();
        QCOMPARE(rufus::CapturePrepareSource(path, request_), 0);
    }

    void acceptsAndDetectsChanges()
    {
        rufus::PrepareIsoSource source(request_);
        QCOMPARE(source.open(getuid()), 0);
        QCOMPARE(source.check(), 0);
        QFile file(request_.imagePath);
        QVERIFY(file.open(QIODevice::Append));
        QCOMPARE(file.write("changed"), 7);
        file.close();
        QCOMPARE(source.check(), ESTALE);
    }

    void rejectsChangedSelection_data()
    {
        QTest::addColumn<QString>("field");
        for (const char* field : {"inode", "modified", "changed", "size", "owner", "symlink", "unreadable"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }

    void rejectsChangedSelection()
    {
        QFETCH(QString, field);
        uid_t caller = getuid();
        int expected = ESTALE;
        if (field == QStringLiteral("inode")) ++request_.imageInode;
        if (field == QStringLiteral("size")) ++request_.imageSize;
        if (field == QStringLiteral("modified")) request_.imageModified = QStringLiteral("0:0");
        if (field == QStringLiteral("changed")) request_.imageChanged = QStringLiteral("0:0");
        if (field == QStringLiteral("owner")) { ++caller; expected = EACCES; }
        if (field == QStringLiteral("unreadable"))
        {
            QVERIFY(QFile::setPermissions(request_.imagePath, QFile::WriteOwner));
            expected = EACCES;
        }
        if (field == QStringLiteral("symlink"))
        {
            const QString link = directory_.filePath(QStringLiteral("link.iso"));
            QVERIFY(QFile::link(request_.imagePath, link));
            request_.imagePath = link;
            expected = ELOOP;
        }
        rufus::PrepareIsoSource source(request_);
        QCOMPARE(source.open(caller), expected);
    }

    void cleanup()
    {
        QFile::remove(directory_.filePath(QStringLiteral("link.iso")));
        QFile::remove(directory_.filePath(QStringLiteral("source.iso")));
    }
};
QTEST_GUILESS_MAIN(TestPrepareSource)
#include "test_prepare_source.moc"
