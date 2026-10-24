// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/application_updates.h"
#include "qt/application_storage.h"
#include "qt/application_language.h"
#include "qt/policy_update_dialog.h"
#include "qt/main_window.h"
#include "qt/selection_controller.h"
#include "http_fixture.h"
#include <QCheckBox>
#include <QDateTime>
#include <QFile>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkProxy>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPointer>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTimer>
#include <cerrno>

using namespace rufus_test;
namespace
{
const QByteArray releasePath = "/repos/mcirsta/rufus/releases?per_page=100&page=";
const QByteArray dateHeader = "Date: Wed, 21 Oct 2026 12:00:00 GMT\r\n";
const QString current = QStringLiteral("4.15.2404-linux-preview.36");
QJsonObject release(bool beta = false)
{
    QJsonObject asset{{"name", "rufus-linux-preview-4.16.2500-linux-preview.40-x86_64.tar.gz"}, {"state", "uploaded"}, {"size", 7000000}};
    return {{"draft", false}, {"prerelease", beta}, {"tag_name", "linux-preview-40"},
        {"html_url", "https://github.com/mcirsta/rufus/releases/tag/linux-preview-40"},
        {"body", "Release notes\n<script>inert plain text</script>"}, {"assets", QJsonArray{asset}}};
}
QJsonObject windowsRelease()
{
    auto value = release();
    value["assets"] = QJsonArray{QJsonObject{{"name", "rufus-9.99.exe"}, {"state", "uploaded"}, {"size", 10000}}};
    return value;
}
QByteArray page(const QJsonArray& releases) { return QJsonDocument(releases).toJson(QJsonDocument::Compact); }
void configure(HttpFixture& server, const QJsonArray& releases)
{
    server.responses[releasePath + "1"] = response(page(releases), dateHeader);
    server.responses["/sbat_level.txt"] = response("shim,999");
    server.responses["/sb_active.txt"] = response(QByteArray(40, 'a'));
    server.responses["/sb_revoked.txt"] = response(QByteArray(40, 'b'));
}
}

class TestApplicationUpdates : public QObject
{
    Q_OBJECT
    QTemporaryDir profile_;
private slots:
    void initTestCase()
    {
        QVERIFY(profile_.isValid()); QString problem;
        QVERIFY(rufus::ConfigureApplicationStorage(profile_.filePath(QStringLiteral("rufus-linux")), true, problem) == 0);
        qputenv("XDG_CACHE_HOME", profile_.filePath(QStringLiteral("cache")).toUtf8());
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    }
    void init()
    {
        rufus::ApplicationSettings settings; settings.clear();
        settings.setValue(QStringLiteral("UpdateCheckInterval"), -1); settings.sync();
    }
    void versions_data()
    {
        QTest::addColumn<QString>("a"); QTest::addColumn<QString>("b"); QTest::addColumn<int>("order");
        QTest::newRow("same") << current << current << 0;
        QTest::newRow("preview") << current << QStringLiteral("4.15.2404-linux-preview.37") << -1;
        QTest::newRow("stable") << current << QStringLiteral("4.15.2404") << -1;
        QTest::newRow("major") << QStringLiteral("5.0.0-linux-preview.1") << current << 1;
        QTest::newRow("minor") << QStringLiteral("4.16.0-linux-preview.1") << current << 1;
        QTest::newRow("build") << QStringLiteral("4.15.2405-linux-preview.1") << current << 1;
        QTest::newRow("numeric") << QStringLiteral("4.9.9999") << QStringLiteral("4.10.1") << -1;
        QTest::newRow("unsigned") << QStringLiteral("65535.65535.65535") << QStringLiteral("32768.0.0") << 1;
    }
    void versions()
    {
        QFETCH(QString, a); QFETCH(QString, b); QFETCH(int, order);
        QCOMPARE(rufus::CompareApplicationVersions(a, b), order);
        QCOMPARE(rufus::CompareApplicationVersions(b, a), -order);
    }
    void malformedVersion_data()
    {
        QTest::addColumn<QString>("text");
        for (const char* value : {"", "v4.15.2404", "4.15", "4.15.2404.exe", "65536.1.1", "4.15.2404-linux-preview.4294967295", "4.15.2404-linux-preview.-1"})
            QTest::newRow(value[0] ? value : "empty") << QString::fromLatin1(value);
    }
    void malformedVersion()
    {
        QFETCH(QString, text); quint16 words[3] = {1, 2, 3}; quint32 preview = 99;
        QVERIFY(!rufus::ParseApplicationVersion(text, words, preview));
        QCOMPARE(words[0], 1); QCOMPARE(preview, 99u);
    }
    void releaseCatalog_data()
    {
        QTest::addColumn<QString>("kind"); QTest::addColumn<bool>("valid"); QTest::addColumn<bool>("found");
        for (const char* kind : {"archive", "package", "both", "stable", "null-notes"})
            QTest::newRow(kind) << QString::fromLatin1(kind) << true << true;
        for (const char* kind : {"empty", "windows", "other-arch", "draft", "beta"})
            QTest::newRow(kind) << QString::fromLatin1(kind) << true << false;
        for (const char* kind : {"invalid-json", "not-array", "channel-type", "wrong-repository", "http-page", "fragment", "size", "size-fraction", "state", "notes-bound", "conflicting", "word-overflow", "catalog-bound"})
            QTest::newRow(kind) << QString::fromLatin1(kind) << false << false;
    }
    void releaseCatalog()
    {
        QFETCH(QString, kind); QFETCH(bool, valid); QFETCH(bool, found);
        auto value = release(); auto asset = value["assets"].toArray()[0].toObject();
        if (kind == "package" || kind == "both") asset["name"] = "rufus-linux-preview-4.16.2500.preview40-1-x86_64.pkg.tar.zst";
        if (kind == "stable") asset["name"] = "rufus-linux-preview-4.16.2500-x86_64.tar.gz";
        if (kind == "other-arch") asset["name"] = "rufus-linux-preview-4.16.2500-linux-preview.40-aarch64.tar.gz";
        if (kind == "size") asset["size"] = -1;
        if (kind == "size-fraction") asset["size"] = 12.5;
        if (kind == "state") asset["state"] = "new";
        if (kind == "word-overflow") asset["name"] = "rufus-linux-preview-65536.1.1-linux-preview.40-x86_64.tar.gz";
        value["assets"] = QJsonArray{asset};
        if (kind == "both") { auto assets = value["assets"].toArray(); assets.append(release()["assets"].toArray()[0]); value["assets"] = assets; }
        if (kind == "null-notes") value["body"] = QJsonValue::Null;
        if (kind == "draft") value["draft"] = true;
        if (kind == "beta") value["prerelease"] = true;
        if (kind == "channel-type") value["prerelease"] = "false";
        if (kind == "wrong-repository") value["html_url"] = "https://github.com/other/rufus/releases/tag/linux-preview-40";
        if (kind == "http-page") value["html_url"] = "http://github.com/mcirsta/rufus/releases/tag/linux-preview-40";
        if (kind == "fragment") value["html_url"] = "https://github.com/mcirsta/rufus/releases/tag/linux-preview-40#fragment";
        if (kind == "notes-bound") value["body"] = QString(32769, QLatin1Char('x'));
        if (kind == "conflicting") {
            asset["name"] = "rufus-linux-preview-4.17.2501.preview41-1-x86_64.pkg.tar.zst";
            auto assets = value["assets"].toArray(); assets.append(asset); value["assets"] = assets;
        }
        if (kind == "windows") value = windowsRelease();
        QByteArray data = page(kind == "empty" ? QJsonArray{} : QJsonArray{value});
        if (kind == "invalid-json") data = "[wrong";
        if (kind == "not-array") data = "{}";
        if (kind == "catalog-bound") data = QByteArray(4 * 1024 * 1024 + 1, ' ');
        rufus::ApplicationRelease newest; int rows = -1; QString problem;
        QCOMPARE(rufus::ParseApplicationReleasePage(data, QStringLiteral("mcirsta/rufus"), QStringLiteral("x86_64"), false, newest, rows, problem), valid);
        QCOMPARE(!newest.version.isEmpty(), found);
        if (found) QVERIFY(newest.page.toString().startsWith(QLatin1String("https://github.com/mcirsta/rufus/releases/tag/")));
        if (!valid) QVERIFY(!problem.isEmpty());
    }
    void selectionAndChannels()
    {
        auto older = release(); auto asset = older["assets"].toArray()[0].toObject();
        asset["name"] = "rufus-linux-preview-4.9.9999-linux-preview.999-x86_64.tar.gz"; older["assets"] = QJsonArray{asset};
        rufus::ApplicationRelease newest; int rows; QString problem;
        QVERIFY(rufus::ParseApplicationReleasePage(page({older, release(true)}), "mcirsta/rufus", "x86_64", false, newest, rows, problem));
        QCOMPARE(newest.version, QStringLiteral("4.9.9999-linux-preview.999"));
        QVERIFY(rufus::ParseApplicationReleasePage(page({release(true)}), "mcirsta/rufus", "x86_64", true, newest, rows, problem));
        QCOMPARE(newest.version, QStringLiteral("4.16.2500-linux-preview.40"));
        const auto before = newest;
        QVERIFY(!rufus::ParseApplicationReleasePage("broken", "mcirsta/rufus", "x86_64", true, newest, rows, problem));
        QCOMPARE(newest.version, before.version); QCOMPARE(newest.page, before.page);
    }
    void publishedRelease()
    {
        const QString inputPath = qEnvironmentVariable("RUFUS_PUBLISHED_RELEASE_INPUT");
        if (inputPath.isEmpty()) return;
        QFile input(inputPath); QVERIFY(input.open(QIODevice::ReadOnly));
        const QByteArray data = input.readAll();
        rufus::ApplicationRelease newest; int rows = 0; QString problem;
        QVERIFY2(rufus::ParseApplicationReleasePage(data, QStringLiteral("mcirsta/rufus"),
            QStringLiteral("x86_64"), true, newest, rows, problem), qPrintable(problem));
        QVERIFY(rows > 0);
        QCOMPARE(newest.version, QStringLiteral("4.15.2404-linux-preview.47"));
        QCOMPARE(newest.page, QUrl(QStringLiteral("https://github.com/mcirsta/rufus/releases/tag/linux-preview.47")));
        QVERIFY(rufus::CompareApplicationVersions(current, newest.version) < 0);
        rufus::ApplicationRelease stable; int stableRows = 0;
        QVERIFY2(rufus::ParseApplicationReleasePage(data, QStringLiteral("mcirsta/rufus"),
            QStringLiteral("x86_64"), false, stable, stableRows, problem), qPrintable(problem));
        QVERIFY(stable.version != newest.version);
        rufus::PolicyUpdateDialog dialog;
        dialog.setApplicationRelease(newest.page); dialog.show();
        auto* link = dialog.findChild<QPushButton*>(QStringLiteral("applicationReleasePage"));
        QVERIFY(link && link->isVisible());
        QSignalSpy requested(&dialog, &rufus::PolicyUpdateDialog::releasePageRequested);
        QTest::mouseClick(link, Qt::LeftButton);
        QCOMPARE(requested.size(), 1); QCOMPARE(requested[0][0].toUrl(), newest.page);
    }
    void networkAndPagination_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"available", "current", "force", "empty", "beta-excluded", "beta-included", "pagination", "page-bound"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void networkAndPagination()
    {
        QFETCH(QString, kind); HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost));
        configure(server, kind == "empty" ? QJsonArray{} : QJsonArray{release(kind.startsWith("beta"))});
        if (kind == "pagination" || kind == "page-bound") {
            QJsonArray windows; for (int index = 0; index < 100; ++index) windows.append(windowsRelease());
            server.responses[releasePath + "1"] = response(page(windows), dateHeader);
            if (kind == "pagination") server.responses[releasePath + "2"] = response(page({release()}), dateHeader);
            else for (int index = 2; index <= 5; ++index) server.responses[releasePath + QByteArray::number(index)] = response(page(windows), dateHeader);
        }
        rufus::ApplicationUpdates updates(nullptr, {server.url("/"), QStringLiteral("mcirsta/rufus")});
        QSignalSpy done(&updates, &rufus::ApplicationUpdates::completed);
        const QString installed = kind == "current" || kind == "force" ? QStringLiteral("4.16.2500-linux-preview.40") : current;
        QCOMPARE(updates.start(installed, "x86_64", kind == "beta-included", kind == "force"), 0);
        QCOMPARE(updates.start(installed, "x86_64", false), EBUSY);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000); QVERIFY(!updates.isActive());
        const auto result = done[0][0].value<rufus::ApplicationUpdateResult>();
        QCOMPARE(result.error, kind == "page-bound" ? E2BIG : 0);
        const bool available = kind == "available" || kind == "force" || kind == "beta-included" || kind == "pagination";
        QCOMPARE(result.available, available);
        if (result.error == 0) QCOMPARE(result.serverTime, 1792584000LL);
        QCOMPARE(server.requests.size(), kind == "page-bound" ? 5 : kind == "pagination" ? 2 : 1);
    }
    void networkRefusals_data()
    {
        QTest::addColumn<QString>("kind"); QTest::addColumn<int>("error");
        QTest::newRow("date") << QStringLiteral("date") << EPROTO;
        QTest::newRow("forbidden") << QStringLiteral("forbidden") << EACCES;
        QTest::newRow("json") << QStringLiteral("json") << EBADMSG;
        QTest::newRow("timeout") << QStringLiteral("timeout") << ETIMEDOUT;
        QTest::newRow("cancel") << QStringLiteral("cancel") << ECANCELED;
    }
    void networkRefusals()
    {
        QFETCH(QString, kind); QFETCH(int, error); HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost));
        configure(server, {release()});
        if (kind == "date") server.responses[releasePath + "1"] = response(page({release()}));
        if (kind == "forbidden") server.responses[releasePath + "1"] = response("denied", dateHeader, -1, 403);
        if (kind == "json") server.responses[releasePath + "1"] = response("broken", dateHeader);
        if (kind == "timeout") server.holdHeaders.insert(releasePath + "1");
        rufus::ApplicationUpdates updates(nullptr, {server.url("/"), "mcirsta/rufus"});
        QSignalSpy done(&updates, &rufus::ApplicationUpdates::completed);
        const int timeoutMs = kind == "timeout" ? 50 : 3500;
        QCOMPARE(updates.start(current, "x86_64", false, false, timeoutMs), 0);
        if (kind == "cancel") updates.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 3000);
        const auto result = done[0][0].value<rufus::ApplicationUpdateResult>();
        QCOMPARE(result.error, error); QVERIFY(!result.available && !result.releaseFound && result.release.page.isEmpty());
        QVERIFY(!updates.isActive());
    }
    void verboseAndForcedDateChecks_data()
    {
        QTest::addColumn<int>("verbose"); QTest::addColumn<bool>("validDate");
        for (int level=0;level<=2;++level) {
            const QByteArray name=QByteArray::number(level);
            QTest::newRow((name+"-valid").constData()) << level << true;
            QTest::newRow((name+"-invalid-date").constData()) << level << false;
        }
    }
    void verboseAndForcedDateChecks()
    {
        QFETCH(int,verbose); QFETCH(bool,validDate);
        HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost)); configure(server,{release()});
        if (!validDate) server.responses[releasePath+"1"]=response(page({release()}));
        rufus::ApplicationUpdates updates(nullptr,{server.url("/"),"mcirsta/rufus"}); updates.setVerbose(verbose);
        QSignalSpy done(&updates,&rufus::ApplicationUpdates::completed);
        QSignalSpy log(&updates,&rufus::ApplicationUpdates::logMessage);
        QCOMPARE(updates.start(current,"x86_64",false,true),0);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,3000);
        QCOMPARE(done[0][0].value<rufus::ApplicationUpdateResult>().error,validDate ? 0 : EPROTO);
        QCOMPARE(log.size(),verbose==0 ? 0 : verbose==1 ? 2 : validDate ? 4 : 3);
        if (verbose>0) QVERIFY(log.last()[0].toString().startsWith("Native update result:"));
    }
    void dueAndPreferences()
    {
        QVERIFY(!rufus::ApplicationUpdateDue(-1, 0, 100)); QVERIFY(!rufus::ApplicationUpdateDue(0, 0, 100));
        QVERIFY(rufus::ApplicationUpdateDue(10, 0, 100)); QVERIFY(!rufus::ApplicationUpdateDue(10, 95, 100));
        QVERIFY(rufus::ApplicationUpdateDue(10, 90, 100)); QVERIFY(rufus::ApplicationUpdateDue(10, 101, 100));
        rufus::PolicyUpdateDialog dialog;
        auto* automatic = dialog.findChild<QCheckBox*>(QStringLiteral("applicationUpdateConsent")); QVERIFY(automatic);
        auto* beta = dialog.findChild<QComboBox*>(QStringLiteral("applicationUpdateBetas")); QVERIFY(beta);
        QVERIFY(!automatic->isChecked() && !automatic->isEnabled()); QCOMPARE(beta->currentIndex(), 1); QVERIFY(!beta->isEnabled());
        auto* frequency = dialog.findChild<QComboBox*>(QStringLiteral("policyUpdateFrequency")); QVERIFY(frequency);
        frequency->setCurrentIndex(frequency->findData(86400)); automatic->setChecked(true);
        beta->setFocus(); QTest::keyClick(beta, Qt::Key_Home);
        QVERIFY(rufus::ApplicationSettings().value(QStringLiteral("ApplicationUpdateConsent"), false).toBool());
        QVERIFY(rufus::ApplicationSettings().value(QStringLiteral("CheckForBetas"), false).toBool());
    }
    void existingPolicyConsentStaysNarrow()
    {
        HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost)); configure(server, {release()});
        { rufus::ApplicationSettings settings;
          settings.setValue(QStringLiteral("UpdateCheckInterval"), 86400);
          settings.setValue(QStringLiteral("LastPolicyUpdateCheck"), QString::number(QDateTime::currentSecsSinceEpoch())); }
        rufus::PolicyUpdateSources policies; policies.textRoot = server.url("/"); policies.dbx.clear();
        rufus::SelectionController controller;
        rufus::MainWindow window(controller, nullptr, {server.url("/"), "mcirsta/rufus"}, policies); window.show();
        auto* timer = window.findChild<QTimer*>(QStringLiteral("applicationUpdateTimer")); QVERIFY(timer);
        QSignalSpy policyDone(window.findChild<rufus::PolicyUpdates*>(), &rufus::PolicyUpdates::completed);
        window.initializePolicyUpdates(); QTRY_COMPARE_WITH_TIMEOUT(policyDone.size(), 1, 3000);
        QVERIFY(!timer->isActive()); QCOMPARE(server.requests.size(), 3);
        QTest::mouseClick(window.findChild<QToolButton*>(QStringLiteral("settingsButton")), Qt::LeftButton);
        auto* consent = window.findChild<QCheckBox*>(QStringLiteral("applicationUpdateConsent")); QVERIFY(consent && !consent->isChecked());
        consent->setChecked(true); QVERIFY(timer->isActive());
        consent->setChecked(false); QVERIFY(!timer->isActive());
        QCOMPARE(server.requests.size(), 3); // Existing policy consent never makes an application request.
    }
    void ordinaryWindowManualCheck_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"available", "failed", "cancelled"}) QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void ordinaryWindowManualCheck()
    {
        QFETCH(QString, kind);
        HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost)); configure(server, {release()});
        if (kind == "failed") server.responses[releasePath + "1"] = response(page({release()}));
        if (kind == "cancelled") { server.holdHeaders.insert(releasePath + "1"); server.holdHeaders.insert("/sbat_level.txt"); }
        { rufus::ApplicationSettings settings; settings.setValue(QStringLiteral("LastApplicationUpdateCheck"), QStringLiteral("123")); }
        rufus::PolicyUpdateSources policies; policies.textRoot = server.url("/"); policies.dbx.clear();
        rufus::SelectionController controller;
        rufus::MainWindow window(controller, nullptr, {server.url("/"), "mcirsta/rufus"}, policies); window.show();
        auto* update = window.findChild<rufus::ApplicationUpdates*>(); QVERIFY(update && !update->isActive());
        QSignalSpy done(update, &rufus::ApplicationUpdates::completed);
        auto* settings = window.findChild<QToolButton*>(QStringLiteral("settingsButton")); QVERIFY(settings);
        QTest::mouseClick(settings, Qt::LeftButton);
        auto* dialog = window.findChild<rufus::PolicyUpdateDialog*>(); QVERIFY(dialog && dialog->isVisible());
        QPointer<QPushButton> check = dialog->findChild<QPushButton*>(QStringLiteral("policyCheckNow")); QVERIFY(check);
        QTest::mouseClick(check, Qt::LeftButton); QVERIFY(!check->isEnabled());
        if (kind == "cancelled") {
            auto* cancel = dialog->findChild<QPushButton*>(QStringLiteral("policyCancelCheck")); QVERIFY(cancel && cancel->isEnabled());
            QTest::mouseClick(cancel, Qt::LeftButton);
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(check && check->isEnabled(), 3000);
        auto* link = dialog->findChild<QPushButton*>(QStringLiteral("applicationReleasePage")); QVERIFY(link);
        QCOMPARE(link->isVisible(), kind == "available");
        QVERIFY(!window.findChild<QMessageBox*>(QStringLiteral("applicationUpdateAvailable"))); // Manual result stays in settings.
        QVERIFY(!rufus::ApplicationSettings().value(QStringLiteral("ApplicationUpdateConsent"), false).toBool());
        QCOMPARE(rufus::ApplicationSettings().value(QStringLiteral("LastApplicationUpdateCheck")).toString(),
            kind == "available" ? QStringLiteral("1792584000") : QStringLiteral("123"));
        dialog->reject(); QCoreApplication::processEvents();
        window.activateWindow();
        auto* select = window.findChild<QPushButton*>(QStringLiteral("selectImageButton")); QVERIFY(select); select->setFocus();
        QTRY_VERIFY(window.isActiveWindow());
        QTest::keyClick(select, Qt::Key_Y, Qt::AltModifier);
        QVERIFY(window.statusBar()->currentMessage().contains(QLatin1String("Force update enabled")));
        QVERIFY(!controller.isBusy());
        QTest::keyClick(select,Qt::Key_Y,Qt::ControlModifier|Qt::AltModifier);
        QVERIFY(window.statusBar()->currentMessage().contains(QLatin1String("Force update disabled")));
        QTest::keyClick(select,Qt::Key_Y,Qt::ControlModifier|Qt::AltModifier);
        QVERIFY(window.statusBar()->currentMessage().contains(QLatin1String("Force update enabled")));
    }
    void automaticNoticeWaitsForDialog()
    {
        HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost)); configure(server, {release()});
        rufus::SelectionController controller;
        rufus::MainWindow window(controller, nullptr, {server.url("/"), "mcirsta/rufus"}); window.show();
        QTest::mouseClick(window.findChild<QToolButton*>(QStringLiteral("settingsButton")), Qt::LeftButton);
        auto* dialog = window.findChild<rufus::PolicyUpdateDialog*>(); QVERIFY(dialog && dialog->isVisible());
        auto* updates = window.findChild<rufus::ApplicationUpdates*>(); QVERIFY(updates);
        QSignalSpy done(updates, &rufus::ApplicationUpdates::completed);
        QCOMPARE(updates->start(current, "x86_64", false), 0);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 3000);
        QVERIFY(!window.findChild<QMessageBox*>(QStringLiteral("applicationUpdateAvailable")));
        dialog->reject();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QMessageBox*>(QStringLiteral("applicationUpdateAvailable")), 3000);
        QPointer<QMessageBox> notice = window.findChild<QMessageBox*>(QStringLiteral("applicationUpdateAvailable"));
        QCOMPARE(notice->defaultButton(), notice->button(QMessageBox::No)); QCOMPARE(notice->textFormat(), Qt::PlainText);
        QVERIFY(notice->detailedText().contains(QLatin1String("<script>inert plain text</script>")));
        QTest::mouseClick(notice->button(QMessageBox::No), Qt::LeftButton);
        QTRY_VERIFY(notice.isNull()); QVERIFY(!controller.isBusy());
    }
    void olderLanguageNotice()
    {
        QString problem; QVERIFY(rufus::SetApplicationLanguage(QStringLiteral("sl-SI"), problem));
        rufus::SelectionController controller; rufus::MainWindow window(controller);
        auto* log = window.findChild<QPlainTextEdit*>(QStringLiteral("logText")); QVERIFY(log);
        QVERIFY(log->toPlainText().contains(QLatin1String("translation requires an update")));
        QVERIFY(rufus::SetApplicationLanguage(QStringLiteral("en-US"), problem));
        QCoreApplication::processEvents();
    }
};
QTEST_MAIN(TestApplicationUpdates)
#include "test_application_updates.moc"
