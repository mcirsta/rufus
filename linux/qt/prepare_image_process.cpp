// SPDX-License-Identifier: GPL-3.0-or-later
#include "prepare_image_process.h"
#include "job_priority.h"
#include "platform/msdos_image.h"
#include "platform/syslinux_resources.h"
#include "application_storage.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <cerrno>
#include <cstring>

namespace rufus
{
namespace
{
bool stageGrubBootFile(const QByteArray& core, PrepareImageRequest& request, const QTemporaryDir& directory)
{
    const bool bounded = request.msDos ? core.size() == DISKCOPY_SIZE
        : core.size() >= qsizetype(PrepareCoreSectorBytes) && quint64(core.size()) <= PrepareCoreMaximumBytes;
    const bool sourceSyslinux = request.syslinux && request.syslinuxSelection == BT_IMAGE
        && !request.imagePath.isEmpty();
    const bool coreJob = request.grub2 || request.grub4dos || request.windowsBios || request.msDos || sourceSyslinux;
    if (!coreJob || request.hasGrub2Core() || !bounded || !directory.isValid())
        return false;
    if (request.msDos) { QVector<MsDosFile> files; if (ReadMsDosFiles(core,files) != 0) return false; }
    const QString path = directory.filePath(request.msDos ? QStringLiteral("diskcopy.dll") : request.grub4dos ? QStringLiteral("grldr") : QStringLiteral("core.img"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Unbuffered))
        return false;
    const qint64 written = file.write(core);
    const bool storedAll = written == core.size() && file.flush();
    if (!storedAll || !file.setPermissions(QFileDevice::ReadOwner))
        return false;
    file.close();
    if (file.error() != QFileDevice::NoError)
        return false;
    if (request.msDos) { request.dosImagePath = path; return true; }
    request.grub2CorePath = path;
    request.grub2CoreSize = quint64(core.size());
    request.grub2CoreSha256 = QCryptographicHash::hash(core, QCryptographicHash::Sha256);
    return true;
}
}

PrepareImageProcess::PrepareImageProcess(const QString& helper, QObject* parent)
    : QObject(parent), helper_(helper)
{
    connect(&process_, &QProcess::readyReadStandardOutput, this, &PrepareImageProcess::readOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, &PrepareImageProcess::readErrors);
    connect(&process_, &QProcess::finished, this, &PrepareImageProcess::processFinished);
    connect(&process_, &QProcess::errorOccurred, this, &PrepareImageProcess::processError);
}

PrepareImageProcess::~PrepareImageProcess()
{
    // EOF reaches the worker's ordinary cancellation path; let it reap mkfs and
    // flush before destroying QProcess. Do not kill a helper to cancel its job.
    disconnect(&process_, nullptr, this, nullptr);
    process_.closeWriteChannel();
    if (request_.isDevice && !ready_)
        process_.terminate();
    if (process_.state() != QProcess::NotRunning)
        process_.waitForFinished(-1);
    closeBadBlocksLog(badBlocksRound_ != 0);
}

bool PrepareImageProcess::isActive() const
{
    return state_ == State::Starting || state_ == State::Running || state_ == State::Cancelling;
}

bool PrepareImageProcess::start(const PrepareImageRequest& request, const QByteArray& grub2Core, const QByteArray& syslinuxResources)
{
    if (isActive())
        return false;
    if (!request.isDevice && !request.imagePath.isEmpty())
        return false;
    PrepareImageRequest staged = request;
    staged.jobPriority = JobPriority();
    std::unique_ptr<QTemporaryDir> directory;
    if (!grub2Core.isEmpty())
    {
        directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/rufus-grub2-XXXXXX"));
        if (!stageGrubBootFile(grub2Core, staged, *directory))
            return false;
    }
    if (!syslinuxResources.isEmpty()) {
        SyslinuxResources resources;
        if (!request.syslinux || request.isSourceFree() || request.hasSyslinuxResources() || DecodeSyslinuxResources(syslinuxResources,resources) != 0) return false;
        if (!directory) directory = std::make_unique<QTemporaryDir>(QDir::tempPath()+QStringLiteral("/rufus-boot-resources-XXXXXX"));
        if (!directory->isValid()) return false;
        const QString path = directory->filePath(QStringLiteral("syslinux.resources"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly|QIODevice::NewOnly) || file.write(syslinuxResources) != syslinuxResources.size() || !file.flush() || !file.setPermissions(QFileDevice::ReadOwner)) return false;
        file.close(); if (file.error() != QFileDevice::NoError) return false;
        staged.syslinuxResourcesPath = path; staged.syslinuxResourcesSize = quint64(syslinuxResources.size());
        staged.syslinuxResourcesSha256 = QCryptographicHash::hash(syslinuxResources,QCryptographicHash::Sha256);
    }
    const QJsonObject object = EncodePrepareRequest(staged);
    PrepareImageRequest checked;
    const bool validRequest = DecodePrepareRequest(object, checked)
        && QJsonDocument(object).toJson(QJsonDocument::Compact).size() < PrepareMessageBytes;
    if (!validRequest || !QDir::isAbsolutePath(helper_))
        return false;
    request_ = std::move(staged);
    coreDirectory_ = std::move(directory);
    const bool standaloneSyslinux = request_.isStandaloneSyslinux();
    phases_ = {QStringLiteral("checking"), QStringLiteral("partitioning"), QStringLiteral("formatting")};
    if (request_.windowsToGo) {
        phases_.append({QStringLiteral("extracting"), QStringLiteral("bootloader")});
        if (request_.windowsFlags != 0) phases_.append(QStringLiteral("customizing"));
        phases_.append(QStringLiteral("finalizing"));
    } else {
        if (standaloneSyslinux && request_.fileSystem == FS_NTFS)
            phases_.append(QStringLiteral("mounting"));
        if (request_.usesUefiNtfs() || request_.isDos() || request_.reactOs || request_.grub2 || request_.grub4dos || request_.windowsBios || request_.kolibriOs || standaloneSyslinux)
            phases_.append(QStringLiteral("bootloader"));
        const bool standaloneUefiNtfs = request_.usesUefiNtfs() && request_.imagePath.isEmpty();
        const bool standaloneGrub2 = request_.grub2 && request_.imagePath.isEmpty();
        if (request_.nonBootable || request_.fileSystem == FS_EXFAT || standaloneUefiNtfs || standaloneGrub2 || request_.reactOs || standaloneSyslinux)
        {
            if (request_.extendedLabel && !(standaloneSyslinux && request_.fileSystem == FS_NTFS))
                phases_.append(QStringLiteral("mounting"));
            phases_.append(QStringLiteral("finalizing"));
        }
        if (!request_.imagePath.isEmpty() || request_.isDos() || request_.grub4dos)
        {
            phases_.append({QStringLiteral("mounting"), QStringLiteral("extracting")});
            if (request_.windowsFlags != 0)
                phases_.append(QStringLiteral("customizing"));
            phases_.append(QStringLiteral("finalizing"));
        }
    }
    if (request_.archive.present()) phases_.append({QStringLiteral("archive"), QStringLiteral("archive-finalizing")});
    copyStepsRemaining_ = 0;
    if (request_.isDos())
    {
        RUFUS_PROGRESS_OPTIONS options = {};
        options.bootable = true;
        options.selection = request_.dosBootType();
        RUFUS_PROGRESS progress;
        InitProgressState(&progress, &options);
        copyStepsRemaining_ = progress.nb_slots[OP_FILE_COPY];
    }
    // Windows reserves three slots but InstallSyslinux emits only two steps.
    // The native CLI reports neither milestone, so both follow verified install.
    if (standaloneSyslinux)
        copyStepsRemaining_ = 2;
    closeBadBlocksLog(false);
    badBlocksRound_ = 0;
    badBlocksPassed_ = badBlocksWaiting_ = badBlocksRetry_ = badBlocksReported_ = false;
    badBlocksErrors_.clear();
    if (request_.badBlocksPattern >= 0 && !openBadBlocksLog()) return false;
    if (request_.badBlocksPattern >= 0) phases_.insert(1, QStringLiteral("badblocks"));
    espRecordWaiting_ = espRecordSeen_ = espRecordApproved_ = false;
    outcome_ = {};
    rawProgress_ = {};
    pending_.clear();
    diagnostics_.clear();
    problem_.clear();
    phase_ = -1;
    phaseName_.clear();
    processed_ = total_ = 0;
    hasProgress_ = false;
    ready_ = sent_ = hasResult_ = cancelled_ = skippedPhase_ = false;
    state_ = State::Starting;
    if (request.isDevice)
        process_.start(QStringLiteral("/usr/bin/pkexec"),
            {QStringLiteral("--disable-internal-agent"), helper_, QStringLiteral("--device")});
    else
        process_.start(helper_, {});
    emit changed();
    return true;
}

void PrepareImageProcess::cancel()
{
    if (!isActive() || cancelled_)
        return;
    cancelled_ = true;
    state_ = State::Cancelling;
    process_.closeWriteChannel();
    // Before authorization, pkexec still has our real UID and can be stopped.
    // After elevation, EOF handles cancellation; no job request precedes ready.
    if (request_.isDevice && !ready_)
        process_.terminate();
    emit changed();
}

void PrepareImageProcess::failProtocol()
{
    if (problem_.isEmpty()) problem_ = tr("The preparation helper returned an invalid or incomplete response.");
    process_.closeWriteChannel();
}

void PrepareImageProcess::accept(const QJsonObject& object)
{
    if (!problem_.isEmpty())
        return;
    const QString event = object.value("event").toString();
    if (event == QStringLiteral("ready") && !ready_ && object.size() == 1)
    {
        ready_ = true;
        if (cancelled_)
            return;
        const QByteArray message = QJsonDocument(EncodePrepareRequest(request_)).toJson(QJsonDocument::Compact) + '\n';
        if (process_.write(message) != message.size())
        {
            failProtocol();
            return;
        }
        sent_ = true;
        state_ = State::Running;
        emit changed();
        return;
    }
    if (!ready_ || hasResult_)
    {
        failProtocol();
        return;
    }
    if (request_.deviceOperation != 0 && event != QStringLiteral("log")) {
        if (event == "phase" && object.size() == 2 && object.value("phase") == "checking" && phase_ == -1) {
            phase_ = 0; emit phaseChanged(QStringLiteral("checking")); return;
        }
        if (event == "esp-record" && request_.deviceOperation == 2 && phase_ == 0 && !espRecordSeen_
            && object.size() == 3 && object.value("restore").isBool()) {
            const QString guid = object.value("guid").toString();
            if (QUuid(guid).isNull() || QUuid(guid).toString() != guid) { failProtocol(); return; }
            espRecordWaiting_ = espRecordSeen_ = true;
            emit espRecordRequired(guid, object.value("restore").toBool()); return;
        }
        if (event == "result" && DecodePrepareOutcome(object, outcome_)) {
            const bool finished = phase_ == 0 && outcome_.started && !espRecordWaiting_
                && (request_.deviceOperation == 1 || espRecordApproved_)
                && outcome_.syncError == 0 && outcome_.cleanupError == 0;
            if (outcome_.error == 0 && !finished) { failProtocol(); return; }
            hasResult_ = true; return;
        }
        failProtocol(); return;
    }
    const bool badBlocksEvent = event.startsWith(QStringLiteral("badblocks-"))
        || (event == QStringLiteral("phase") && object.value("phase") == "badblocks")
        || (event == QStringLiteral("progress") && phaseName_ == QStringLiteral("badblocks"));
    if (badBlocksEvent)
    {
        if (!acceptBadBlocks(object)) failProtocol();
        return;
    }
    if (event == QStringLiteral("result") && object.value("error").toInt(-1) == 0
        && request_.badBlocksPattern >= 0 && !badBlocksPassed_)
    {
        failProtocol(); return;
    }
    if (request_.rawWrite && event != QStringLiteral("log"))
    {
        if (!acceptRaw(object)) failProtocol();
        return;
    }
    if (event == QStringLiteral("result") && DecodePrepareOutcome(object, outcome_))
    {
        const bool extentFits = outcome_.offset <= request_.size && outcome_.size <= request_.size - outcome_.offset;
        const bool expectedLayout = request_.sfd
            ? outcome_.offset == 0 && outcome_.size == request_.size
            : outcome_.offset != 0;
        const bool complete = outcome_.started && expectedLayout && extentFits
            && outcome_.filesystemBytes > 0 && outcome_.filesystemBytes <= outcome_.size;
        const bool cleanExit = outcome_.syncError == 0 && outcome_.cleanupError == 0
            && phase_ + 1 == phases_.size() && !skippedPhase_;
        const bool copiedFiles = outcome_.copiedBytes > 0 && outcome_.copiedBytes <= outcome_.filesystemBytes;
        const bool completeDosCopy = !request_.isDos() || (copyStepsRemaining_ == 0 && copiedFiles);
        const bool standaloneSyslinux = request_.isStandaloneSyslinux();
        const bool standaloneGrub2 = request_.grub2 && request_.imagePath.isEmpty();
        const bool completeBootCopy = !(standaloneSyslinux || standaloneGrub2)
            || (copyStepsRemaining_ == 0 && outcome_.copiedBytes == 0);
        const bool biosImage = (request_.grub2 || request_.grub4dos || request_.windowsBios || request_.kolibriOs) && !request_.imagePath.isEmpty();
        const bool completeImageCopy = !biosImage || copiedFiles;
        const bool standaloneGrub4dos = request_.grub4dos && request_.isSourceFree();
        const bool completeGrub4dosCopy = !standaloneGrub4dos
            || (copiedFiles && outcome_.copiedBytes == request_.grub2CoreSize);
        const bool completeCopy = completeDosCopy && completeBootCopy && completeImageCopy && completeGrub4dosCopy;
        const bool appearance = !request_.extendedLabel || outcome_.appearanceDone;
        const bool unsolicitedAppearance = !request_.extendedLabel && outcome_.appearanceDone;
        const bool archive = !request_.archive.present() || outcome_.archiveDone;
        const bool unsolicitedArchive = !request_.archive.present() && (outcome_.archiveDone || outcome_.archiveBytes != 0);
        if (unsolicitedAppearance || unsolicitedArchive
            || (outcome_.error == 0 && (!complete || !cleanExit || !completeCopy || !appearance || !archive)))
            failProtocol();
        else
            hasResult_ = true;
        return;
    }
    if (event == QStringLiteral("step") && sent_)
    {
        const bool dosCopy = request_.isDos() && phaseName_ == QStringLiteral("extracting");
        const bool syslinuxCopy = request_.isStandaloneSyslinux() && phaseName_ == QStringLiteral("bootloader");
        const bool copyPhase = dosCopy || syslinuxCopy;
        const bool copyStep = object.size() == 2 && object.value("operation") == QJsonValue(OP_FILE_COPY);
        if (!copyPhase || !copyStep || copyStepsRemaining_ <= 0)
        {
            failProtocol();
            return;
        }
        --copyStepsRemaining_;
        emit stepChanged(OP_FILE_COPY);
        return;
    }
    if (event == QStringLiteral("progress") && sent_)
    {
        int operation;
        quint64 processed, total;
        if (DecodePrepareProgress(object, operation, processed, total))
        {
            const bool copying = !request_.isDos() && operation == OP_FILE_COPY && phaseName_ == QStringLiteral("extracting");
            const bool patching = operation == OP_PATCH && phaseName_ == QStringLiteral("customizing");
            const bool consistent = !hasProgress_ || (total == total_ && processed >= processed_);
            const bool formatting = operation == OP_FORMAT && phaseName_ == QStringLiteral("formatting") && total == 100;
            const bool archive = request_.archive.present() && operation == OP_EXTRACT_ZIP && phaseName_ == QStringLiteral("archive");
            if ((copying || patching || formatting || archive) && consistent)
            {
                processed_ = processed;
                total_ = total;
                hasProgress_ = true;
                emit progressChanged(operation, processed, total);
                return;
            }
        }
        failProtocol();
        return;
    }
    if (event == QStringLiteral("log") && object.size() == 2 && object.value("message").isString())
    {
        emit logMessage(object.value("message").toString());
        return;
    }
    if (event == QStringLiteral("phase") && object.size() == 2 && sent_)
    {
        const QString name = object.value("phase").toString();
        const int next = phases_.indexOf(name);
        const bool cleanupStarted = request_.windowsToGo
            ? phase_ >= phases_.indexOf(QStringLiteral("bootloader"))
            : phases_.contains(QStringLiteral("mounting")) && phase_ >= phases_.indexOf(QStringLiteral("mounting"));
        const bool cleanupAfterFailure = next >= 0 && name == QStringLiteral("finalizing")
            && cleanupStarted && phase_ < next;
        const bool tested = request_.badBlocksPattern < 0 || badBlocksPassed_ || phase_ < 1;
        if (((next == phase_ + 1 && next >= 0) || cleanupAfterFailure) && tested)
        {
            skippedPhase_ = skippedPhase_ || next != phase_ + 1;
            phase_ = next;
            phaseName_ = name;
            processed_ = total_ = 0;
            hasProgress_ = false;
            emit phaseChanged(name);
            return;
        }
    }
    failProtocol();
}

bool PrepareImageProcess::acceptRaw(const QJsonObject& object)
{
    const bool ffu = request_.imageCompression == IMG_COMPRESSION_FFU;
    const quint64 imageBytes = ffu ? request_.size : PrepareRawWriteBytes(request_);
    const bool fastZero = request_.zeroMode == ZERO_FAST;
    const bool sparseVtsi = fastZero || request_.imageCompression == BLED_COMPRESSION_VTSI || ffu;
    if (!sent_)
        return false;
    const QString event = object.value("event").toString();
    if (event == QStringLiteral("phase"))
    {
        const bool tested = request_.badBlocksPattern >= 0 && badBlocksPassed_
            && phaseName_ == QStringLiteral("badblocks") && object.size() == 2
            && object.value("phase") == "writing";
        if (tested) { phaseName_ = QStringLiteral("writing"); return true; }
        if (phase_ != -1 || object.size() != 2 || object.value("phase") != "checking")
            return false;
        phase_ = 0;
        phaseName_ = QStringLiteral("checking");
        emit phaseChanged(QStringLiteral("checking"));
        return true;
    }
    if (phase_ != 0)
        return false;
    if (event == QStringLiteral("raw-progress"))
    {
        if (request_.badBlocksPattern >= 0 && !badBlocksPassed_) return false;
        int phase;
        quint64 processed, total;
        if (!DecodeRawProgress(object, phase, processed, total))
            return false;
        const bool nextPhase = phase == rawProgress_.phase + 1;
        const bool samePhase = phase == rawProgress_.phase;
        if (!nextPhase && !samePhase)
            return false;
        if (phase == RAW_WRITE_SYNC)
        {
            if (!nextPhase || processed != 0 || total != 0 || rawProgress_.bytes_written == 0)
                return false;
        }
        else
        {
            const quint64 previous = phase == RAW_WRITE_COPY ? rawProgress_.bytes_written : rawProgress_.bytes_verified;
            if (total != imageBytes || processed < previous || (nextPhase && processed != 0))
                return false;
            if (phase == RAW_WRITE_VERIFY && rawProgress_.bytes_written != imageBytes)
                return false;
            if (phase == RAW_WRITE_COPY) rawProgress_.bytes_written = processed;
            else rawProgress_.bytes_verified = processed;
        }
        rawProgress_.phase = RAW_WRITE_PHASE(phase);
        emit rawProgressChanged(phase, processed, total);
        return true;
    }
    PrepareImageOutcome decoded;
    if (!DecodeRawOutcome(object, decoded))
        return false;
    const RAW_WRITE_RESULT& raw = decoded.raw;
    const bool physicalCountsFit = raw.bytes_written <= imageBytes && raw.bytes_verified <= raw.bytes_written;
    const bool countersFit = sparseVtsi ? physicalCountsFit
        : physicalCountsFit && raw.bytes_written >= rawProgress_.bytes_written
            && raw.bytes_verified >= rawProgress_.bytes_verified;
    const bool beforeWriting = raw.phase == RAW_WRITE_PREPARE;
    if (!countersFit || (beforeWriting && (raw.bytes_written != 0 || rawProgress_.phase != RAW_WRITE_PREPARE)))
        return false;
    if (decoded.error == 0)
    {
        if (request_.badBlocksPattern >= 0 && !badBlocksPassed_) return false;
        const bool complete = sparseVtsi ? raw.phase == RAW_WRITE_DONE && (fastZero || raw.bytes_written > 0)
            && raw.bytes_verified == raw.bytes_written && rawProgress_.bytes_written == imageBytes
            : raw.phase == RAW_WRITE_DONE && raw.bytes_written == imageBytes
                && raw.bytes_verified == imageBytes;
        const bool verifiedProgress = rawProgress_.phase == RAW_WRITE_VERIFY
            && rawProgress_.bytes_verified == imageBytes;
        if (!complete || !verifiedProgress || decoded.syncError != 0)
            return false;
    }
    outcome_ = decoded;
    hasResult_ = true;
    return true;
}

bool PrepareImageProcess::openBadBlocksLog()
{
    const QString directory = reportDirectory_.isEmpty() ? ApplicationReportDirectory() : reportDirectory_;
    if (!QDir().mkpath(directory)) {
        problem_ = tr("Cannot create the bad-block test log directory: %1").arg(directory);
        return false;
    }
    const QString name = QStringLiteral("rufus_%1_XXXXXX.log").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    badBlocksLog_ = std::make_unique<QTemporaryFile>(QDir(directory).filePath(name));
    badBlocksLog_->setAutoRemove(false);
    if (!badBlocksLog_->open())
    {
        problem_ = tr("Cannot create the bad-block test log: %1").arg(badBlocksLog_->errorString());
        badBlocksLog_.reset();
        return false;
    }
    return writeBadBlocksLog(QStringLiteral("Rufus bad-block test — %1\nDevice: %2\nPattern: %3; fake-capacity detection: %4\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODate), request_.path)
        .arg(request_.badBlocksPattern).arg(request_.detectFakes ? 1 : 0).toUtf8());
}

bool PrepareImageProcess::writeBadBlocksLog(const QByteArray& bytes)
{
    if (badBlocksLog_ && badBlocksLog_->write(bytes) == bytes.size() && badBlocksLog_->flush()) return true;
    problem_ = tr("Cannot write the bad-block test log.");
    process_.closeWriteChannel();
    return false;
}

void PrepareImageProcess::closeBadBlocksLog(bool retain)
{
    if (!badBlocksLog_) return;
    const QString path = badBlocksLog_->fileName();
    badBlocksLog_->close();
    badBlocksLog_.reset();
    if (retain) emit logMessage(tr("Bad-block test log: %1").arg(path));
    else QFile::remove(path);
}

void PrepareImageProcess::answerBadBlocks(int choice)
{
    if (!isActive() || !badBlocksWaiting_ || choice < BadBlocksAbort || choice > BadBlocksIgnore) return;
    badBlocksWaiting_ = false;
    const char* names[] = {"abort", "retry", "ignore"};
    const QByteArray frame = QJsonDocument(QJsonObject{{"badBlocks", names[choice]}, {"round", badBlocksRound_}})
        .toJson(QJsonDocument::Compact) + '\n';
    if (choice == BadBlocksRetry && !openBadBlocksLog()) return;
    badBlocksRetry_ = choice == BadBlocksRetry;
    badBlocksPassed_ = choice == BadBlocksIgnore;
    if (process_.write(frame) != frame.size()) failProtocol();
}

bool PrepareImageProcess::acceptBadBlocks(const QJsonObject& object)
{
    if (!sent_ || request_.badBlocksPattern < 0) return false;
    const QString event = object.value("event").toString();
    if (event == QStringLiteral("phase"))
    {
        const bool first = badBlocksRound_ == 0 && phaseName_ == QStringLiteral("checking");
        if (object.size() != 2 || (!first && !badBlocksRetry_)) return false;
        if (!request_.rawWrite && first) ++phase_;
        ++badBlocksRound_;
        badBlocksRetry_ = badBlocksPassed_ = badBlocksReported_ = false;
        badBlocksErrors_.clear(); badBlocksReport_ = {};
        phaseName_ = QStringLiteral("badblocks");
        processed_ = total_ = 0; hasProgress_ = false;
        emit phaseChanged(phaseName_);
        return true;
    }
    if (phaseName_ != QStringLiteral("badblocks") || badBlocksRound_ == 0 || badBlocksReported_) return false;
    const int pattern = request_.badBlocksPattern;
    const quint64 passes = pattern == 0 ? 1 : pattern == 1 ? 2 : 4;
    const quint64 maximum = request_.size * passes * 2;
    if (event == QStringLiteral("progress"))
    {
        int operation; quint64 processed, total;
        if (!DecodePrepareProgress(object, operation, processed, total) || operation != OP_BADBLOCKS) return false;
        if (total != maximum || (hasProgress_ && processed < processed_)) return false;
        processed_ = processed; total_ = total; hasProgress_ = true;
        emit progressChanged(operation, processed, total); return true;
    }
    if (object.value("round") != QJsonValue(badBlocksRound_)) return false;
    if (event == QStringLiteral("badblocks-error"))
    {
        const QString text = object.value("block").toString();
        bool valid = false; const quint64 block = text.toULongLong(&valid);
        const int type = object.value("type").toInt(-1);
        const quint64 blocks = (request_.size + BADBLOCK_BLOCK_SIZE - 1) / BADBLOCK_BLOCK_SIZE;
        if (object.size() != 4 || !valid || QString::number(block) != text || block >= blocks
            || type < 0 || type > 2 || badBlocksErrors_.contains(block) || badBlocksErrors_.size() >= BB_BAD_BLOCKS_THRESHOLD) return false;
        badBlocksErrors_.insert(block, type);
        return writeBadBlocksLog(QStringLiteral("Block %1: %2 error\n").arg(block).arg(type == 0 ? "read" : type == 1 ? "write" : "corruption").toUtf8());
    }
    BadBlocksReport report;
    if (!DecodeBadBlocksReport(object, report) || report.count != quint32(badBlocksErrors_.size())
        || report.written > maximum || report.read > maximum) return false;
    quint32 counts[3] = {};
    for (int type : std::as_const(badBlocksErrors_)) ++counts[type];
    if (report.readErrors != counts[0] || report.writeErrors != counts[1] || report.corruptionErrors != counts[2]) return false;
    const bool complete = hasProgress_ && processed_ == maximum;
    if (report.error == 0 && report.count == 0 && !complete) return false;
    if (!writeBadBlocksLog(QStringLiteral("%1 bad blocks; read %2, write %3, corruption %4; error %5\n")
        .arg(report.count).arg(report.readErrors).arg(report.writeErrors).arg(report.corruptionErrors).arg(report.error).toUtf8())) return false;
    const QString path = badBlocksLog_->fileName();
    closeBadBlocksLog(report.count != 0 || (report.error != 0 && report.written != 0));
    badBlocksReport_ = report; badBlocksReported_ = true;
    badBlocksPassed_ = report.error == 0 && report.count == 0;
    badBlocksWaiting_ = report.error == 0 && report.count != 0;
    if (badBlocksWaiting_) emit badBlocksReviewRequired(report, path);
    return true;
}

void PrepareImageProcess::readOutput()
{
    while (process_.bytesAvailable() > 0)
    {
        pending_ += process_.read(PrepareMessageBytes);
        qsizetype newline;
        while ((newline = pending_.indexOf('\n')) >= 0)
        {
            const QJsonDocument document = QJsonDocument::fromJson(pending_.left(newline));
            if (newline >= PrepareMessageBytes || !document.isObject())
                failProtocol();
            else
                accept(document.object());
            pending_.remove(0, newline + 1);
        }
        if (pending_.size() >= PrepareMessageBytes)
        {
            pending_.clear();
            failProtocol();
        }
    }
}

void PrepareImageProcess::readErrors()
{
    process_.setReadChannel(QProcess::StandardError);
    while (process_.bytesAvailable() > 0)
    {
        const QByteArray bytes = process_.read(PrepareMessageBytes);
        diagnostics_ += bytes.left(PrepareMessageBytes - diagnostics_.size());
    }
    process_.setReadChannel(QProcess::StandardOutput);
}

void PrepareImageProcess::processFinished(int exitCode, QProcess::ExitStatus status)
{
    readOutput();
    readErrors();
    const bool normalExit = status == QProcess::NormalExit;
    const bool cancelledBeforeRequest = request_.isDevice && cancelled_ && !sent_;
    const bool authorizationStopped = request_.isDevice && !ready_ && normalExit
        && (exitCode == 126 || exitCode == 127);
    if (authorizationStopped || cancelledBeforeRequest)
    {
        // 127 also covers absent agents and generic denial. Only an explicit
        // caller cancellation or pkexec's dismissed status is labelled cancelled.
        outcome_.error = cancelled_ || exitCode == 126 ? ECANCELED : EACCES;
        hasResult_ = true;
        problem_ = tr("Device authorization was cancelled, denied or unavailable.");
    }
    const bool expectedExit = hasResult_ && exitCode == (outcome_.error == 0 ? 0 : 1);
    if (!cancelledBeforeRequest && (!pending_.isEmpty() || !normalExit || (!expectedExit && !authorizationStopped)))
        failProtocol();
    if (!problem_.isEmpty() && !authorizationStopped && !cancelledBeforeRequest)
    {
        outcome_.error = EIO;
        // Without a trustworthy result, a sent request may have changed media.
        outcome_.started = outcome_.started || sent_;
    }
    if (outcome_.error == 0 && cancelled_)
        outcome_.error = ECANCELED;
    const bool cleanupSucceeded = outcome_.syncError == 0 && outcome_.cleanupError == 0;
    const bool cancelled = outcome_.error == ECANCELED && cleanupSucceeded;
    if (cancelled)
        state_ = State::Cancelled;
    else if (outcome_.error != 0 || !cleanupSucceeded)
        state_ = State::Failed;
    else
        state_ = State::Complete;
    if (problem_.isEmpty() && outcome_.syncError != 0)
        problem_ = tr("Could not flush the prepared image: %1").arg(QString::fromLocal8Bit(strerror(outcome_.syncError)));
    if (problem_.isEmpty() && outcome_.cleanupError != 0)
        problem_ = tr("Could not finish device cleanup: %1").arg(QString::fromLocal8Bit(strerror(outcome_.cleanupError)));
    else if (problem_.isEmpty() && outcome_.error != 0)
        problem_ = QString::fromLocal8Bit(strerror(outcome_.error));
    closeBadBlocksLog(badBlocksRound_ != 0);
    coreDirectory_.reset();
    if (!diagnostics_.isEmpty())
        emit logMessage(QString::fromLocal8Bit(diagnostics_));
    emit changed();
    emit finished();
}

void PrepareImageProcess::processError(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart)
        return; // Running-process failures are finalized when the child is reaped.
    closeBadBlocksLog(false);
    outcome_.error = EIO;
    problem_ = process_.errorString();
    state_ = State::Failed;
    coreDirectory_.reset();
    emit changed();
    emit finished();
}
void PrepareImageProcess::answerEspRecord(bool accepted)
{
    if (!espRecordWaiting_ || !isActive()) return;
    espRecordWaiting_ = false; espRecordApproved_ = accepted;
    const QByteArray message = QJsonDocument(QJsonObject{{"espRecord", accepted}}).toJson(QJsonDocument::Compact) + '\n';
    if (process_.write(message) != message.size()) failProtocol();
}

void PrepareImageProcess::setJobPriority(int priority)
{
    if (priority < MinimumJobPriority || priority > MaximumJobPriority || cancelled_ || hasResult_) return;
    request_.jobPriority = priority;
    if (!isActive() || !sent_) return;
    const QByteArray command = QJsonDocument(QJsonObject{{"priority", priority}}).toJson(QJsonDocument::Compact) + '\n';
    if (process_.write(command) != command.size()) failProtocol();
}

}
