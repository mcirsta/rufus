// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/image_mode_dialog.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QTest>

namespace
{

enum DismissAction { CancelButton, EscapeKey, CloseWindow };

IMAGE_WRITE_CHOICES hybridChoices(bool esp)
{
    RUFUS_IMG_REPORT report = {};
    report.is_iso = 1;
    report.is_bootable_img = 1;
    report.has_efi = 1 << ARCH_X86_64;
    report.projected_size = MAX_ISO_TO_ESP_SIZE - 1;
    return GetImageWriteChoices(&report, 0, esp, true);
}

// Read the first (English) definition directly from Rufus's existing messages.
// This checks copied UI text against the original without a new runtime loader.
QString originalMessage(int id, const QStringList& arguments = {})
{
    QFile file(QString::fromUtf8(RUFUS_LOCALE_SOURCE));
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Cannot read Rufus's original messages");
    const QString prefix = QStringLiteral("t MSG_%1 ").arg(id, 3, 10, QLatin1Char('0'));
    QString message;
    bool found = false;
    while (!file.atEnd())
    {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (!found && !line.startsWith(prefix))
            continue;
        if (found && !line.startsWith(QLatin1Char('"')))
            break;
        found = true;
        message += line.mid(line.indexOf(QLatin1Char('"')) + 1).chopped(1);
    }
    if (!found)
        qFatal("Missing original message");
    message.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
    for (const QString& argument : arguments)
    {
        const int offset = message.indexOf(QStringLiteral("%s"));
        if (offset < 0)
            qFatal("Unexpected original message arguments");
        message.replace(offset, 2, argument);
    }
    return message;
}

} // namespace

class TestImageModeDialog : public QObject
{
    Q_OBJECT

private slots:
    void originalMessagesAndDefault_data()
    {
        QTest::addColumn<bool>("esp");
        QTest::newRow("ISO-DD") << false;
        QTest::newRow("ISO-ESP-DD") << true;
    }

    void originalMessagesAndDefault()
    {
        QFETCH(bool, esp);
        QWidget parent;
        parent.show();
        rufus::ImageModeDialog dialog(hybridChoices(esp), &parent);
        const QList<QRadioButton*> choices = dialog.findChildren<QRadioButton*>();
        QLabel* message = dialog.findChild<QLabel*>(QStringLiteral("imageModeMessage"));
        QDialogButtonBox* actions = dialog.findChild<QDialogButtonBox*>();
        QVERIFY(message && actions);
        const QString iso = originalMessage(36);
        const QString dd = originalMessage(95);
        QCOMPARE(dialog.windowTitle(), originalMessage(274, {QStringLiteral("ISOHybrid")}));
        QCOMPARE(message->text(), originalMessage(275, {iso, dd, iso, dd}));
        QCOMPARE(message->textFormat(), Qt::PlainText);
        QVERIFY(message->wordWrap());
        QCOMPARE(choices.size(), esp ? 3 : 2);
        QCOMPARE(choices.first()->text(), originalMessage(276, {iso}));
        QCOMPARE(choices.last()->text(), originalMessage(277, {dd}));
        if (esp)
            QCOMPARE(choices.at(1)->text(), originalMessage(277, {QStringLiteral("ISO → ESP")}));
        QVERIFY(choices.first()->isChecked());
        QCOMPARE(actions->standardButtons(), QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        QVERIFY(actions->button(QDialogButtonBox::Ok)->isDefault());
        QCOMPARE(dialog.selectedMode(), IMAGE_WRITE_NONE);
        QSignalSpy finished(&dialog, &QDialog::finished);
        dialog.open();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QCOMPARE(dialog.windowModality(), Qt::WindowModal);
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_MODE_SCREENSHOT");
        if (!esp && !screenshot.isEmpty())
            QVERIFY(dialog.grab().save(screenshot));
        QTest::keyClick(&dialog, Qt::Key_Return);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.selectedMode(), IMAGE_WRITE_ISO);
    }

    void everyRowMapsToItsMode_data()
    {
        QTest::addColumn<bool>("esp");
        QTest::addColumn<int>("row");
        QTest::addColumn<int>("mode");
        QTest::newRow("two-ISO") << false << 0 << int(IMAGE_WRITE_ISO);
        QTest::newRow("two-DD") << false << 1 << int(IMAGE_WRITE_DD);
        QTest::newRow("three-ISO") << true << 0 << int(IMAGE_WRITE_ISO);
        QTest::newRow("three-ESP") << true << 1 << int(IMAGE_WRITE_ESP);
        QTest::newRow("three-DD") << true << 2 << int(IMAGE_WRITE_DD);
    }

    void everyRowMapsToItsMode()
    {
        QFETCH(bool, esp);
        QFETCH(int, row);
        QFETCH(int, mode);
        rufus::ImageModeDialog dialog(hybridChoices(esp));
        dialog.open();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        const QList<QRadioButton*> choices = dialog.findChildren<QRadioButton*>();
        QRadioButton* selected = choices.at(row);
        // Qt's radio hit area covers the indicator/text, not stretched padding.
        QTest::mouseClick(selected, Qt::LeftButton, Qt::NoModifier,
            QPoint(selected->sizeHint().width() / 2, selected->height() / 2));
        for (int i = 0; i < choices.size(); i++)
            QCOMPARE(choices.at(i)->isChecked(), i == row);
        QCOMPARE(dialog.selectedMode(), IMAGE_WRITE_NONE); // Clicking a radio is not acceptance.
        QTest::mouseClick(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
        QCOMPARE(dialog.selectedMode(), IMAGE_WRITE_MODE(mode));
    }

    void cancelDoesNotChooseAMode_data()
    {
        QTest::addColumn<int>("action");
        QTest::newRow("Cancel") << int(CancelButton);
        QTest::newRow("Escape") << int(EscapeKey);
        QTest::newRow("Close") << int(CloseWindow);
    }

    void cancelDoesNotChooseAMode()
    {
        QFETCH(int, action);
        rufus::ImageModeDialog dialog(hybridChoices(true));
        dialog.open();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QRadioButton* dd = dialog.findChildren<QRadioButton*>().last();
        QTest::mouseClick(dd, Qt::LeftButton, Qt::NoModifier,
            QPoint(dd->sizeHint().width() / 2, dd->height() / 2));
        QVERIFY(dd->isChecked());
        QSignalSpy finished(&dialog, &QDialog::finished);
        if (action == CancelButton)
            QTest::mouseClick(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel), Qt::LeftButton);
        else if (action == EscapeKey)
            QTest::keyClick(&dialog, Qt::Key_Escape);
        else
            dialog.close();
        QCOMPARE(finished.count(), 1);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QCOMPARE(dialog.selectedMode(), IMAGE_WRITE_NONE);

        rufus::ImageModeDialog retry(hybridChoices(true));
        QVERIFY(retry.findChildren<QRadioButton*>().first()->isChecked());
        QCOMPARE(retry.selectedMode(), IMAGE_WRITE_NONE);
    }

    void keyboardAndLargeFontLayout()
    {
        rufus::ImageModeDialog dialog(hybridChoices(true));
        QFont font = dialog.font();
        font.setPointSizeF(font.pointSizeF() * 1.5);
        dialog.setFont(font);
        dialog.open();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        const QList<QRadioButton*> choices = dialog.findChildren<QRadioButton*>();
        choices.first()->setFocus();
        QTest::keyClick(choices.first(), Qt::Key_Down);
        QVERIFY(choices.at(1)->isChecked());
        QTest::keyClick(choices.at(1), Qt::Key_Down);
        QVERIFY(choices.last()->isChecked());
        for (QRadioButton* choice : choices)
        {
            QVERIFY(dialog.rect().contains(choice->geometry()));
            QVERIFY(choice->width() >= choice->sizeHint().width());
            QVERIFY(choice->height() >= choice->sizeHint().height());
        }
        QLabel* message = dialog.findChild<QLabel*>(QStringLiteral("imageModeMessage"));
        QVERIFY(dialog.rect().contains(message->geometry()));
        QVERIFY(message->height() >= message->heightForWidth(message->width()));
        QVERIFY(message->geometry().bottom() < choices.first()->geometry().top());
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_MODE_LAYOUT_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(dialog.grab().save(screenshot));
        QTest::keyClick(choices.last(), Qt::Key_Return);
        QCOMPARE(dialog.selectedMode(), IMAGE_WRITE_DD);
    }

    void espQuestionAndOriginalMessage()
    {
        const IMAGE_WRITE_CHOICES modes = {{IMAGE_WRITE_ISO, IMAGE_WRITE_ESP}, 2};
        rufus::ImageModeDialog dialog(modes);
        const auto choices = dialog.findChildren<QRadioButton*>();
        QCOMPARE(choices.size(), 2);
        QVERIFY(choices.first()->isChecked());
        QVERIFY(choices.first()->isEnabled());
        QVERIFY(choices.at(1)->isEnabled());
        QCOMPARE(dialog.windowTitle(), originalMessage(274, {QStringLiteral("ESP")}));
        QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("imageModeMessage"))->text(), originalMessage(310));
        choices.at(1)->setChecked(true);
        dialog.accept();
        QCOMPARE(dialog.selectedMode(), IMAGE_WRITE_ESP);
    }
};

QTEST_MAIN(TestImageModeDialog)
#include "test_image_mode_dialog.moc"
