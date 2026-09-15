// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/prepare_image_protocol.h"
#include "qt/rufus_job.h"
#include "platform/fat_format.h"
#include "platform/partition_image.h"
#include "platform/prepare_device.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
constexpr int sectorBytes = 512;
constexpr int sectorsPerTrack = 63;
constexpr int requestTimeoutMs = 120000;
constexpr int inputReadsPerEvent = 16;
constexpr int logCharacters = 256;

int checkFile(int fd, const rufus::PrepareImageRequest& request)
{
    struct stat file, path;
    if (fstat(fd, &file) != 0 || lstat(QFile::encodeName(request.path).constData(), &path) != 0)
        return errno;
    const bool ordinaryFile = S_ISREG(file.st_mode) && S_ISREG(path.st_mode);
    const bool ownedFile = file.st_uid == getuid() && file.st_nlink == 1;
    if (!ordinaryFile || !ownedFile)
        return ENOTSUP;
    const bool sameIdentity = quint64(file.st_dev) == request.device && quint64(file.st_ino) == request.inode;
    const bool samePath = path.st_dev == file.st_dev && path.st_ino == file.st_ino;
    const bool sameSize = file.st_size >= 0 && quint64(file.st_size) == request.size;
    if (!sameIdentity || !samePath || !sameSize)
        return ESTALE;
    return QThread::currentThread()->isInterruptionRequested() ? ECANCELED : 0;
}

class PrepareWorker final : public rufus::RufusJob
{
    Q_OBJECT
public:
    explicit PrepareWorker(const rufus::PrepareImageRequest& request) : request_(request) {}

signals:
    void phase(QString phase);
    void result(rufus::PrepareImageOutcome outcome);

protected:
    void execute() override
    {
        rufus::PrepareImageOutcome outcome;
        emit phase(QStringLiteral("checking"));
        if (request_.isDevice)
        {
            rufus::PrepareDeviceTarget target(request_);
            outcome.error = target.open();
            if (outcome.error == 0)
                prepare(target.descriptor(), outcome, &target);
        }
        else
        {
            const QByteArray path = QFile::encodeName(request_.path);
            const int fd = open(path.constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (fd < 0)
                outcome.error = errno;
            else
            {
                outcome.error = checkFile(fd, request_);
                if (outcome.error == 0 && flock(fd, LOCK_EX | LOCK_NB) != 0)
                    outcome.error = errno;
                if (outcome.error == 0)
                    prepare(fd, outcome);
                if (close(fd) != 0 && outcome.error == 0)
                    outcome.error = errno;
            }
        }
        emit result(outcome);
    }

private:
    void prepare(int fd, rufus::PrepareImageOutcome& outcome, rufus::PrepareDeviceTarget* device = nullptr)
    {
        GPT_IMAGE_OPTIONS layout = {request_.size, sectorBytes, sectorsPerTrack, request_.clusterSize,
            request_.isDevice ? request_.device : 0};
        GPT_IMAGE_RESULT partition = {};
        FAT32_FORMAT_OPTIONS format = {};
        FAT32_FORMAT_RESULT formatted = {};
        const QByteArray label = request_.label.toUtf8();
        // Check both operations before GPT writes, using the adapters' own rules.
        if (label.size() > FAT32_LABEL_LENGTH)
        {
            outcome.error = EINVAL;
            return;
        }
        outcome.error = PlanGptImagePartition(fd, &layout, &partition);
        if (outcome.error != 0)
            return;
        format.offset = partition.offset;
        format.size = partition.size;
        format.cluster_size = request_.clusterSize;
        format.volume_id = request_.volumeId;
        memcpy(format.label, label.constData(), size_t(label.size()));
        outcome.error = device != nullptr ? CheckFat32FormatOptions(&format)
            : CheckFat32ImageRequest(fd, RUFUS_MKFS_FAT_PATH, &format);
        if (outcome.error == 0 && access(RUFUS_MKFS_FAT_PATH, X_OK) != 0)
            outcome.error = errno;
        if (outcome.error != 0)
            return;
        outcome.error = device != nullptr ? device->check() : checkFile(fd, request_);
        if (outcome.error != 0)
            return;
        emit phase(QStringLiteral("partitioning"));
        outcome.error = CreateGptImagePartition(fd, &layout, &partition);
        outcome.started = partition.started;
        outcome.syncError = partition.sync_error;
        outcome.offset = partition.offset;
        outcome.size = partition.size;
        if (outcome.error == 0)
            outcome.error = device != nullptr ? device->check() : checkFile(fd, request_);
        if (outcome.error != 0)
            return;
        emit phase(QStringLiteral("formatting"));
        outcome.error = device != nullptr ? device->format(format, RUFUS_MKFS_FAT_PATH, formatted)
            : FormatFat32Image(fd, RUFUS_MKFS_FAT_PATH, &format, &formatted);
        outcome.syncError = formatted.sync_error;
        outcome.filesystemBytes = formatted.filesystem_bytes;
        if (outcome.error == 0)
            outcome.error = device != nullptr ? device->check() : checkFile(fd, request_);
    }

    rufus::PrepareImageRequest request_;
};

class PrepareServer : public QObject
{
    Q_OBJECT
public:
    explicit PrepareServer(bool deviceMode) : input_(STDIN_FILENO, QSocketNotifier::Read), deviceMode_(deviceMode)
    {
        connect(&input_, &QSocketNotifier::activated, this, &PrepareServer::readInput);
        connect(&thread_, &QThread::finished, this, &PrepareServer::finish);
        timer_.setSingleShot(true);
        connect(&timer_, &QTimer::timeout, this, &PrepareServer::requestExpired);
        timer_.start(requestTimeoutMs);
        send({{"event", "ready"}});
    }

    ~PrepareServer() override
    {
        thread_.requestInterruption();
        thread_.quit();
        thread_.wait();
    }

private:
    void send(const QJsonObject& object)
    {
        if (outputClosed_)
            return;
        const QByteArray message = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
        // Every frame fits PIPE_BUF: a nonblocking pipe write is atomic or fails.
        ssize_t count = -1;
        if (message.size() <= rufus::PrepareMessageBytes)
        {
            do { count = write(STDOUT_FILENO, message.constData(), size_t(message.size())); }
            while (count < 0 && errno == EINTR);
        }
        if (count != message.size())
        {
            outputClosed_ = true;
            stop(ECANCELED);
        }
    }

    void stop(int error)
    {
        input_.setEnabled(false);
        timer_.stop();
        if (stopError_ == 0)
            stopError_ = error;
        if (started_)
            thread_.requestInterruption();
        else
            QTimer::singleShot(0, this, &PrepareServer::finish);
    }

    void requestExpired() { stop(ETIMEDOUT); }
    void phase(const QString& name) { send({{"event", "phase"}, {"phase", name}}); }
    void log(const QString& message) { send({{"event", "log"}, {"message", message.left(logCharacters)}}); }
    void result(const rufus::PrepareImageOutcome& outcome) { outcome_ = outcome; hasResult_ = true; }

    void readInput()
    {
        char bytes[512];
        for (int attempt = 0; attempt < inputReadsPerEvent; ++attempt)
        {
            const ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && errno == EAGAIN)
                return;
            if (count <= 0)
            {
                stop(count == 0 ? ECANCELED : errno);
                return;
            }
            // One request only. Closing stdin cancels; further data is invalid.
            if (started_)
            {
                stop(EPROTO);
                return;
            }
            buffer_.append(bytes, qsizetype(count));
            if (buffer_.size() > rufus::PrepareMessageBytes)
            {
                stop(EMSGSIZE);
                return;
            }
            const qsizetype newline = buffer_.indexOf('\n');
            if (newline < 0)
                continue;
            rufus::PrepareImageRequest request;
            const QJsonDocument document = QJsonDocument::fromJson(buffer_);
            const bool oneRequest = newline == buffer_.size() - 1 && document.isObject();
            if (!oneRequest || !rufus::DecodePrepareRequest(document.object(), request))
            {
                stop(EPROTO);
                return;
            }
            if (request.isDevice != deviceMode_)
            {
                stop(EPERM);
                return;
            }
            buffer_.clear();
            timer_.stop();
            PrepareWorker* worker = new PrepareWorker(request);
            worker->moveToThread(&thread_);
            connect(&thread_, &QThread::started, worker, &rufus::RufusJob::run);
            connect(worker, &PrepareWorker::phase, this, &PrepareServer::phase);
            connect(worker, &rufus::RufusJob::logMessage, this, &PrepareServer::log);
            connect(worker, &rufus::RufusJob::failed, this, &PrepareServer::log);
            connect(worker, &PrepareWorker::result, this, &PrepareServer::result);
            connect(worker, &rufus::RufusJob::finished, &thread_, &QThread::quit, Qt::DirectConnection);
            connect(&thread_, &QThread::finished, worker, &QObject::deleteLater);
            started_ = true;
            thread_.start();
        }
    }

    void finish()
    {
        thread_.wait();
        if (stopError_ != 0)
            outcome_.error = stopError_;
        else if (!hasResult_)
        {
            outcome_.error = EIO;
            outcome_.started = started_; // No result: conservatively report possible writes.
        }
        send(rufus::EncodePrepareOutcome(outcome_));
        QCoreApplication::exit(outcome_.error == 0 ? 0 : 1);
    }

    QSocketNotifier input_;
    QTimer timer_;
    QThread thread_;
    QByteArray buffer_;
    rufus::PrepareImageOutcome outcome_;
    int stopError_ = 0;
    bool started_ = false;
    bool hasResult_ = false;
    bool outputClosed_ = false;
    const bool deviceMode_;
};
}

int main(int argc, char** argv)
{
    const bool deviceMode = argc == 2 && strcmp(argv[1], "--device") == 0;
    const bool ordinaryUser = getuid() != 0 && getuid() == geteuid() && getgid() == getegid();
    bool callerValid = false;
    const quint64 caller = qEnvironmentVariable("PKEXEC_UID").toULongLong(&callerValid);
    const bool authorizedRoot = deviceMode && getuid() == 0 && geteuid() == 0
        && callerValid && caller > 0 && caller < UINT32_MAX;
    const bool fileMode = argc == 1 && ordinaryUser;
    if (!fileMode && !authorizedRoot)
        return 77;
    if (deviceMode)
    {
        // Native validation must query the real system service, never a bus
        // address supplied by the invoking user's environment.
        qunsetenv("DBUS_SYSTEM_BUS_ADDRESS");
        qunsetenv("DBUS_SESSION_BUS_ADDRESS");
    }
    struct stat input, output;
    if (fstat(STDIN_FILENO, &input) != 0 || fstat(STDOUT_FILENO, &output) != 0)
        return 77;
    if (!S_ISFIFO(input.st_mode) || !S_ISFIFO(output.st_mode))
        return 77;
    signal(SIGPIPE, SIG_IGN);
    if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) != 0 || fcntl(STDOUT_FILENO, F_SETFL, O_NONBLOCK) != 0)
        return 77;
    QCoreApplication application(argc, argv);
    PrepareServer server(deviceMode);
    return application.exec();
}

#include "prepare_main.moc"
