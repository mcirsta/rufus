// SPDX-License-Identifier: GPL-3.0-or-later
#include "about_dialog.h"
#include "rufus_build_info.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QVBoxLayout>

// Reuse the original license and notices, which Windows displays as RTF.
#define RUFUS_URL "https://rufus.ie"
#include "../../src/license.h"
#undef RUFUS_URL

namespace rufus
{

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("aboutDialog"));
    setWindowTitle(tr("About Rufus"));
    setWindowModality(Qt::WindowModal);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* description = new QLabel(tr(
        "<h2>Rufus Linux preview</h2>"
        "<p>Version %1</p>"
        "<p>Create bootable USB drives from disk and ISO images.</p>"
        "<p>Based on Rufus %2.<br>Copyright © 2011–2026 Pete Batard.</p>"
        "<p>This Linux preview is under development. The complete Windows "
        "feature set is still being ported.</p>"
        "<p><a href=\"https://rufus.ie\">Rufus website</a> · "
        "<a href=\"https://github.com/pbatard/rufus\">Upstream source</a></p>")
        .arg(QLatin1String(RUFUS_LINUX_VERSION), QLatin1String(RUFUS_UPSTREAM_VERSION)), this);
    description->setObjectName(QStringLiteral("aboutDescription"));
    description->setTextFormat(Qt::RichText);
    description->setWordWrap(true);
    description->setOpenExternalLinks(true);
    description->setTextInteractionFlags(Qt::TextBrowserInteraction);
    layout->addWidget(description);
    QTabWidget* tabs = new QTabWidget(this);
    QPlainTextEdit* license = new QPlainTextEdit(QString::fromUtf8(gplv3), tabs);
    license->setObjectName(QStringLiteral("licenseText"));
    license->setReadOnly(true);
    license->setTabChangesFocus(true);
    tabs->addTab(license, tr("License"));
    // This fixed upstream notice contains only an RTF wrapper and line breaks.
    // Preserve all its text without maintaining a second copyright list.
    QString notices = QString::fromUtf8(additional_copyrights);
    notices.remove(QStringLiteral("{\\rtf1\\ansi\n"));
    notices.replace(QStringLiteral("\\line\n"), QStringLiteral("\n"));
    if (notices.endsWith(QLatin1Char('}')))
        notices.chop(1);
    QPlainTextEdit* credits = new QPlainTextEdit(notices, tabs);
    credits->setObjectName(QStringLiteral("creditsText"));
    credits->setReadOnly(true);
    credits->setTabChangesFocus(true);
    tabs->addTab(credits, tr("Credits"));
    layout->addWidget(tabs);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    resize(600, 600);
}

} // namespace rufus
