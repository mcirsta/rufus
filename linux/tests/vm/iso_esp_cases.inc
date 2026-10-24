// SPDX-License-Identifier: GPL-3.0-or-later
// Included in the PID-1/DMI/root guarded persistence driver; disposable /dev/sda only.
void runIsoEspJob(const QDBusConnection& bus, int filesystem, bool persistence)
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Rufus"), QStringLiteral("Linux"));
    settings.setValue(QStringLiteral("UseExtVersion"), 3); settings.sync();
    for (const char* name : {"/run/format-exit-held", "/run/format-exit-release", "/run/format-launches"})
        QFile::remove(QString::fromLatin1(name));
    rufus::SelectionController controller;
    QObject::connect(&controller, &rufus::SelectionController::logMessage, logMessage);
    controller.isoWriter().configure(bus, QStringLiteral("/usr/bin/rufus-prepare-helper"));
    controller.writer().setConnection(bus); controller.writer().setHelper(QStringLiteral("/usr/bin/rufus-prepare-helper"));
    rufus::MainWindow window(controller);
    controller.setDiscovery({publishDevice(bus, diskBytes, false)}, {}); window.show();
    controller.selectImage(QStringLiteral("/debian.iso"));
    QElapsedTimer timer; timer.start();
    while (controller.isAnalyzing() && timer.elapsed() < timeoutMs) QTest::qWait(10);
    require(!controller.isAnalyzing() && controller.canSetPersistence(), "ESP fresh regular-EFI/live source");
    controller.setPartitionScheme(rufus::PartitionScheme::Gpt); controller.setFileSystem(filesystem);
    const quint32 layoutCluster = filesystem == FS_FAT16 ? 65536 : 1024;
    controller.setClusterSize(layoutCluster); controller.setQuickFormat(filesystem == FS_FAT16);
    controller.setVolumeLabel(QStringLiteral("RUFUS_ESP"));
    if (persistence) controller.setPersistenceSize(persistenceBytes);
    require(controller.selection().clusterSize == layoutCluster, "ESP explicit layout allocation unit");
    auto* start = window.findChild<QPushButton*>(QStringLiteral("startButton"));
    auto* progress = window.findChild<QProgressBar*>(QStringLiteral("progressBar"));
    require(start && progress && start->isEnabled(), "ESP START available");
    QTest::mouseClick(start, Qt::LeftButton);
    auto& writer = controller.isoWriter();
    require(writer.state() == rufus::IsoWriteState::ChoosingMode, "ESP question before preflight/authorization");
    auto* dialog = window.findChild<rufus::ImageModeDialog*>(QStringLiteral("imageModeDialog"));
    require(dialog && dialog->isVisible(), "ordinary ESP selection dialog");
    auto* esp = dialog->findChild<QRadioButton*>(QStringLiteral("imageModeChoice2"));
    require(esp && esp->isEnabled(), "ESP choice enabled");
    esp->setChecked(true);
    QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
    timer.restart();
    while (writer.state() == rufus::IsoWriteState::Checking && timer.elapsed() < timeoutMs) QTest::qWait(10);
    if (writer.state() != rufus::IsoWriteState::AwaitingConfirmation)
        qFatal("ESP preflight: %s", qPrintable(writer.statusText()));
    require(writer.request().isoEsp && !writer.request().mbr && writer.request().quickFormat
        && writer.request().clusterSize == layoutCluster, "frozen ESP choice, layout unit and formatter quick mode");
    require(writer.request().persistenceSize == (persistence ? persistenceBytes : 0), "frozen ESP persistence");
    require(readFile(QStringLiteral("/run/format-launches")).isEmpty(), "ESP no authorization before confirmation");
    auto* confirmation = window.findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
    require(confirmation && confirmation->isVisible(), "ESP destructive confirmation");
    QTest::mouseClick(confirmation->button(QMessageBox::Ok), Qt::LeftButton); timer.restart();
    while (readFile(QStringLiteral("/run/format-exit-held")).isEmpty() && timer.elapsed() < timeoutMs) QTest::qWait(10);
    require(!readFile(QStringLiteral("/run/format-exit-held")).isEmpty(), "ESP helper result and cleanup");
    require(writer.isActive() && progress->value() < MAX_PROGRESS, "ESP READY waits for joined exit");
    require(QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-*")}, QDir::Dirs).isEmpty(), "ESP private mounts/seeds cleaned");
    writeFile(QStringLiteral("/run/format-exit-release"), "release\n"); timer.restart();
    while (writer.isActive() && timer.elapsed() < timeoutMs) QTest::qWait(10);
    if (writer.state() != rufus::IsoWriteState::Complete) qFatal("ESP creation: %s", qPrintable(writer.statusText()));
    require(progress->value() == MAX_PROGRESS && progress->format() == QStringLiteral("READY")
        && readFile(QStringLiteral("/run/format-launches")) == "launch\n", "ESP one authorization and full joined READY");
    QFile disk(QStringLiteral("/dev/sda")); require(disk.open(QIODevice::ReadOnly), "ESP independent GPT readback");
    require(disk.seek(1024), "ESP main entry location"); const QByteArray entry = disk.read(128);
    require(entry.size() == 128 && entry.first(16) == QByteArray::fromHex("28732ac11ff8d211ba4b00a0c93ec93b")
        && qFromLittleEndian<quint64>(entry.constData() + 48) == 0, "ESP actual GUID and attributes");
    const quint64 offset = qFromLittleEndian<quint64>(entry.constData() + 32) * 512;
    require(offset == writer.check().partition.offset && disk.seek(qint64(offset)), "ESP original layout retained");
    const QByteArray bpb = disk.read(512);
    const unsigned formatCluster = filesystem == FS_FAT16 ? 32768 : 4096;
    require(bpb.size() == 512 && quint8(bpb[13]) * 512u == formatCluster
        && qFromLittleEndian<quint32>(bpb.constData() + 28) == offset / 512, "ESP formatter defaults and hidden sectors");
    disk.close();
    const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(claim >= 0 && close(claim) == 0, "ESP every device claim released");
    writeFile(QStringLiteral("/run/check-esp"), QByteArray::number(filesystem) + ' ' + QByteArray::number(int(persistence)) + '\n');
    QProcess inspector; inspector.setProcessChannelMode(QProcess::ForwardedChannels);
    inspector.start(QStringLiteral("/usr/bin/bash"), {QStringLiteral("/inspect-esp")});
    require(inspector.waitForFinished(timeoutMs) && inspector.exitCode() == 0, "ESP complete independent file/persistence readback");
    printf("RUFUS_ISO_ESP_JOB FS=%d PERSIST=%d LAYOUT_CLUSTER=%u FORMAT_CLUSTER=%u AUTH=1 HELD=1 READY=100\n",
        filesystem, int(persistence), layoutCluster, formatCluster);
}
