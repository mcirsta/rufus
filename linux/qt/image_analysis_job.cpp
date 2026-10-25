// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_analysis_job.h"
#include "platform/rufus_reporting.h"
#include "syslinux_version.h"
#include "image_policy.h"
#include "platform/image_source.h"
#include "platform/prepare_source.h"
#include "platform/compressed_write.h"
#include "platform/compressed_ffu.h"
#include "platform/windows_to_go.h"
#include "platform/secure_boot.h"

#include <cdio/logging.h>
#include <QFile>
#include <QFileInfo>
#include <iterator>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>

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
    const int error = wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8 | WIMLIB_INIT_FLAG_DEFAULT_CASE_INSENSITIVE);
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
    IMAGE_FILE_SOURCE file = {-1, {}, false, false, nullptr, 0};
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
    for (size_t i = 0; i < std::size(paths); i++)
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

int collectFfuStore(void* context, unsigned index, uint64_t bytes, unsigned sector,
                    const uint8_t* devicePath, size_t pathBytes)
{
    Q_UNUSED(sector);
    try
    {
        QString path;
        for (size_t i = 0; i < pathBytes; i += 2)
            path.append(QChar(quint16(devicePath[i]) | quint16(devicePath[i + 1]) << 8));
        auto* stores = static_cast<QStringList*>(context);
        stores->append(QStringLiteral("Store %1 — %2 bytes%3").arg(index).arg(bytes)
            .arg(path.isEmpty() ? QString() : QStringLiteral(" — %1").arg(path.left(120))));
        return 0;
    }
    catch (const std::bad_alloc&)
    {
        return ENOMEM;
    }
}

} // namespace

ImageAnalysisJob::ImageAnalysisJob(quint64 request, const QString& path, quint64 size,
                                 const QDateTime& modified, bool ignoreBootMarker,
                                 const PrepareImageRequest& selectedSource, bool expertMode, const SecureBootPolicy& policy, bool isoSupport)
    : request_(request), path_(path), size_(size), modified_(modified),
      wimInitError_(initializeWimLibrary()), ignoreBootMarker_(ignoreBootMarker), expertMode_(expertMode), isoSupport_(isoSupport), selectedSource_(selectedSource), policy_(policy)
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
    const bool sameSize = info.size() > 0 && quint64(info.size()) == size_;
    const bool sameModification = info.lastModified() == modified_;
    return info.isFile() && sameSize && sameModification;
}

void ImageAnalysisJob::execute()
{
    ImageAnalysisResult result;
    ISO_ANALYSIS_OPTIONS options = {selectedSource_.rockRidge, selectedSource_.joliet, {0, 0}};
    ImageSourceScope source;
    const QByteArray path = QFile::encodeName(path_);
    if (IsIsoScanCancelled())
        result.status = ISO_SCAN_CANCELLED;
    else if (!imageUnchanged())
        result.problem = tr("The image changed or was removed. Select it again.");
    else if (OpenImageFileSource(path.constData(), &source.file) != 0)
        result.problem = tr("Cannot retain the image for analysis. Select it again.");
    else
    {
        const int compression = GetImageCompressionType(path.constData());
        if (compression != BLED_COMPRESSION_NONE)
        {
            COMPRESSED_WRITE_PLAN plan = {};
            source.file.ffu_store = selectedSource_.ffuStore;
            int error = 0;
            if (selectedSource_.ffuStore != 0)
                error = CheckPrepareSourceIdentity(selectedSource_, source.file);
            if (error == 0 && compression == IMG_COMPRESSION_FFU && selectedSource_.ffuStore == 0)
                error = ReadFfuStoreList(&source.file, collectFfuStore, &result.ffuStores);
            const bool chooseStore = error == 0 && result.ffuStores.size() > 1;
            // Admit each provider to ordinary USB creation after guest qualification.
            const bool admitted = compression == BLED_COMPRESSION_GZIP || compression == BLED_COMPRESSION_XZ
                || compression == BLED_COMPRESSION_BZIP2 || compression == BLED_COMPRESSION_LZMA
                || compression == BLED_COMPRESSION_LZW || compression == BLED_COMPRESSION_ZIP
                || compression == BLED_COMPRESSION_ZSTD || compression == BLED_COMPRESSION_VTSI
                || compression == IMG_COMPRESSION_FFU
                || compression == IMG_COMPRESSION_VHD || compression == IMG_COMPRESSION_VHDX;
            if (error == 0 && !chooseStore)
                error = admitted ? PrepareCompressedWrite(&source.file, path.constData(),
                    std::numeric_limits<qint64>::max(), COMPRESSED_BOOT_SECTOR_BYTES, ignoreBootMarker_, &plan) : ENOTSUP;
            if (chooseStore)
            {
                error = CapturePrepareSource(path_, result.source);
                if (error == 0) error = CheckPrepareSourceIdentity(result.source, source.file);
                if (error == 0) result.status = ISO_SCAN_OK;
                else result.ffuStores.clear();
            }
            else if (error == 0)
            {
                result.source.ignoreBootMarker = ignoreBootMarker_;
                result.source.imageCompression = compression;
                result.source.ffuStore = compression == IMG_COMPRESSION_FFU ? plan.image.ffu_store : 0;
                result.source.decodedImageSize = plan.image.decoded_bytes;
                result.source.ffuMinimumSize = plan.image.ffu_minimum_bytes;
                // Analysis geometry is canonical only. START freezes the actual
                // selected device sector size before confirmation/authorization.
                result.source.imageSectorSize = plan.sector_size;
                result.source.decodedImageSha256 = QByteArray(reinterpret_cast<const char*>(plan.decoded_sha256), SHA256_HASHSIZE);
                if (compression == BLED_COMPRESSION_VTSI || compression == IMG_COMPRESSION_FFU)
                    result.source.imageLayoutSha256 = QByteArray(reinterpret_cast<const char*>(plan.layout_sha256), SHA256_HASHSIZE);
                const int captured = CapturePrepareSource(path_, result.source);
                if (captured == 0 && CheckPrepareSourceIdentity(result.source, source.file) == 0)
                {
                    result.status = ISO_SCAN_OK;
                    result.rawImage = result.ddRequired = true;
                    result.analysis.report.is_bootable_img = plan.image.has_boot_marker || compression == IMG_COMPRESSION_FFU ? 1 : 2;
                    result.analysis.report.compression_type = compression;
                    result.analysis.report.image_size = size_;
                    result.analysis.report.projected_size = plan.image.decoded_bytes;
                }
                else
                {
                    result = {};
                    result.problem = tr("The image changed during analysis. Select it again.");
                }
            }
            else if (error == ENOTSUP)
                result.problem = tr("This compressed image is unsupported or has no Rufus boot marker (55 AA).");
            else if (error != ECANCELED)
                result.problem = tr("Cannot validate the compressed image: %1").arg(QString::fromLocal8Bit(strerror(error)));
        }
        else if (wimInitError_ != 0)
            result.problem = tr("Cannot initialize the Windows image reader: %1")
                .arg(QString::fromUtf8(wimlib_get_error_string(static_cast<wimlib_error_code>(wimInitError_))));
        else if (source.file.is_wim)
        {
            result.status = ScanWindowsStandaloneImage(source.file, path.constData(), result.analysis, result.windows);
            if (result.status == ISO_SCAN_OK || result.status == ISO_SCAN_PARTIAL)
            {
                if (CapturePrepareSource(path_, result.source) != 0)
                {
                    result = {};
                    result.problem = tr("Cannot retain the Windows image source identity. Select it again.");
                }
            }
            else if (result.status != ISO_SCAN_CANCELLED)
                result.problem = tr("Cannot read the Windows image metadata or EFI boot manager. See the log for details.");
        }
        else if (isoSupport_)
        {
            CdioLogScope logging;
            readEmbeddedVersions(options);
            uprintf("ISO analysis: %s", path.constData());
            result.status = AnalyzeIsoImageWithSource(path.constData(), size_, &options,
                                                     &result.analysis, &source.value);
            if (result.status == ISO_SCAN_OK || result.status == ISO_SCAN_PARTIAL)
            {
                result.status = ScanWindowsImage(&source.value, &result.analysis, &result.windows);
                if (result.status == ISO_SCAN_OK || result.status == ISO_SCAN_PARTIAL) {
                    SecureBootPolicy policy = policy_;
                    const int policyError = policy.dbx.isEmpty() ? LoadBundledSecureBootPolicy(policy) : 0;
                    if (policyError == 0)
                        result.status = ScanIsoSecureBoot(source.value, result.analysis, policy, expertMode_);
                    else
                        uprintf("  Warning: Could not load the bundled Secure Boot policy.");
                }
                result.analysis.report.is_bootable_img = source.file.has_boot_marker ? 1 : ignoreBootMarker_ ? 2 : 0;
                result.source.ignoreBootMarker = ignoreBootMarker_;
                result.ddRequired = ApplyIsoPolicy(&result.analysis.report);
                if (CapturePrepareSource(path_, result.source) != 0)
                {
                    result = {};
                    result.problem = tr("Cannot retain the analyzed source identity. Select it again.");
                }
            }
        }
        // Windows recognizes a raw DD image by its inspected bytes even when
        // its suffix is .iso or unknown. A readable ISO keeps its ISO/DD policy.
        const bool rawSectors = size_ >= COMPRESSED_BOOT_SECTOR_BYTES && size_ % COMPRESSED_BOOT_SECTOR_BYTES == 0;
        const bool rawCandidate = rawSectors && !source.file.is_wim && (source.file.has_boot_marker || ignoreBootMarker_);
        if (compression == BLED_COMPRESSION_NONE && result.status == ISO_SCAN_ERROR
            && !IsIsoScanCancelled() && rawCandidate)
        {
            result = {};
            if (CapturePrepareSource(path_, result.source) == 0
                && CheckPrepareSourceIdentity(result.source, source.file) == 0)
            {
                result.source.ignoreBootMarker = ignoreBootMarker_;
                result.status = ISO_SCAN_OK;
                result.rawImage = result.ddRequired = true;
                result.analysis.report.is_bootable_img = source.file.has_boot_marker ? 1 : 2;
                result.analysis.report.image_size = result.analysis.report.projected_size = size_;
            }
            else
                result.problem = tr("The image changed during analysis. Select it again.");
        }
        if (result.status != ISO_SCAN_CANCELLED && (!imageUnchanged()
            || CheckImageFileSource(&source.file, path.constData()) != 0))
        {
            result = {};
            result.problem = tr("The image changed during analysis. Select it again.");
        }
        else if (result.status == ISO_SCAN_ERROR && result.problem.isEmpty())
            result.problem = tr("Cannot analyze this image as UDF or ISO9660. See the log for details.");
    }
    if (IsIsoScanCancelled())
    {
        result = {};
        result.status = ISO_SCAN_CANCELLED;
    }
    if (!result.rawImage && !result.source.imagePath.isEmpty()) {
        result.source.rockRidge = selectedSource_.rockRidge;
        result.source.joliet = selectedSource_.joliet;
    }
    emit analysisReady(request_, result);
}

} // namespace rufus
