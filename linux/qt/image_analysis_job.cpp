// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_analysis_job.h"
#include "platform/rufus_reporting.h"
#include "syslinux_version.h"
#include "image_policy.h"
#include "platform/image_source.h"

#include <cdio/logging.h>
#include <QFile>
#include <QFileInfo>

static void initializeImageResources()
{
    static bool initialized = false;
    if (!initialized)
    {
        Q_INIT_RESOURCE(image_resources);
        initialized = true;
    }
}

namespace rufus
{
namespace
{

class WimLibrary
{
public:
    const int error = wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8);
    ~WimLibrary()
    {
        wimlib_global_cleanup();
    }
};

int initializeWimLibrary()
{
    // Initialize before jobs move to workers. Controllers join their workers
    // before process teardown destroys this one library instance.
    static const WimLibrary library;
    return library.error;
}

struct ImageSourceScope
{
    ISO_ANALYSIS_SOURCE value = {};
    IMAGE_FILE_SOURCE file = {-1, {}, false, false};
    ~ImageSourceScope()
    {
        CloseIsoAnalysisSource(&value);
        CloseImageFileSource(&file);
    }
};

void logCdio(cdio_log_level_t level, const char* message)
{
    (void)level;
    uprintfs(message);
}

// libcdio has a process-wide handler. RufusJob's guard covers this scope;
// restoring it also handles exceptions from the operation.
class CdioLogScope
{
public:
    CdioLogScope()
        : previous_(cdio_log_set_handler(logCdio)), level_(cdio_loglevel_default)
    {
        cdio_loglevel_default = CDIO_LOG_WARN;
    }

    ~CdioLogScope()
    {
        cdio_log_set_handler(previous_);
        cdio_loglevel_default = level_;
    }

private:
    cdio_log_handler_t previous_;
    cdio_log_level_t level_;
};

void readEmbeddedVersions(ISO_ANALYSIS_OPTIONS& options)
{
    const char* paths[] = {":/rufus/syslinux/ldlinux_v4.sys", ":/rufus/syslinux/ldlinux_v6.sys"};
    for (int i = 0; i < 2; i++)
    {
        QFile file(QString::fromLatin1(paths[i]));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        QByteArray data = file.readAll();
        char* suffix = nullptr;
        // QByteArray owns a writable extra NUL, as required by the shared parser.
        options.embedded_sl_version[i] = GetSyslinuxVersion(data.data(), size_t(data.size()) + 1, &suffix);
    }
}

} // namespace

ImageAnalysisJob::ImageAnalysisJob(quint64 request, const QString& path, quint64 size,
                                 const QDateTime& modified)
    : request_(request), path_(path), size_(size), modified_(modified),
      wimInitError_(initializeWimLibrary())
{
    initializeImageResources();
    qRegisterMetaType<ImageAnalysisResult>();
    connect(this, &RufusJob::logMessage, this, &ImageAnalysisJob::forwardLog, Qt::DirectConnection);
    connect(this, &RufusJob::failed, this, &ImageAnalysisJob::reportFailure, Qt::DirectConnection);
}

void ImageAnalysisJob::forwardLog(QString message)
{
    emit analysisLog(request_, message);
}

void ImageAnalysisJob::reportFailure(QString message)
{
    ImageAnalysisResult result;
    result.problem = message;
    emit analysisReady(request_, result);
}

bool ImageAnalysisJob::imageUnchanged() const
{
    const QFileInfo info(path_);
    return info.isFile() && info.size() > 0 && quint64(info.size()) == size_
        && info.lastModified() == modified_;
}

void ImageAnalysisJob::execute()
{
    ImageAnalysisResult result;
    ISO_ANALYSIS_OPTIONS options = {true, true, {0, 0}};
    ImageSourceScope source;
    const QByteArray path = QFile::encodeName(path_);
    if (IsIsoScanCancelled())
        result.status = ISO_SCAN_CANCELLED;
    else if (wimInitError_ != 0)
        result.problem = tr("Cannot initialize the Windows image reader: %1")
            .arg(QString::fromUtf8(wimlib_get_error_string(static_cast<wimlib_error_code>(wimInitError_))));
    else if (!imageUnchanged())
        result.problem = tr("The image changed or was removed. Select it again.");
    else if (OpenImageFileSource(path.constData(), &source.file) != 0)
        result.problem = tr("Cannot retain the image for analysis. Select it again.");
    else
    {
        CdioLogScope logging;
        readEmbeddedVersions(options);
        uprintf("ISO analysis: %s", path.constData());
        result.status = AnalyzeIsoImageWithSource(path.constData(), size_, &options,
                                                 &result.analysis, &source.value);
        if (result.status == ISO_SCAN_OK || result.status == ISO_SCAN_PARTIAL)
        {
            result.status = ScanWindowsImage(&source.value, &result.analysis, &result.windows);
            result.analysis.report.is_bootable_img = source.file.has_boot_marker ? 1 : 0;
            result.ddRequired = ApplyIsoPolicy(&result.analysis.report);
        }
        if (result.status != ISO_SCAN_CANCELLED && (!imageUnchanged()
            || CheckImageFileSource(&source.file, path.constData()) != 0))
        {
            result = {};
            result.problem = tr("The image changed during analysis. Select it again.");
        }
        else if (result.status == ISO_SCAN_ERROR)
            result.problem = tr("Cannot analyze this image as UDF or ISO9660. See the log for details.");
    }
    if (IsIsoScanCancelled())
    {
        result = {};
        result.status = ISO_SCAN_CANCELLED;
    }
    emit analysisReady(request_, result);
}

} // namespace rufus
