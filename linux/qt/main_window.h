// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QMainWindow>
#include <QElapsedTimer>
#include <QTimer>
#include <memory>
#include "application_log.h"
#include "desktop_controls.h"
#include "policy_updates.h"
#include "application_updates.h"

class QCloseEvent;
class QAction;
class QMenu;
class QDragEnterEvent;
class QDropEvent;
class QDialog;
class QFileDialog;
class QLabel;
class QMessageBox;
class QPlainTextEdit;

namespace Ui
{
class RufusWindow;
}

namespace rufus
{

class SelectionController;
struct BadBlocksReport;
class ImageModeDialog;
class WindowsUserDialog;
class WindowsImageDialog;
class FreeDosLocaleDialog;
class PolicyUpdateDialog;
class ImageDownloadDialog;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(SelectionController& controller, QWidget* parent = nullptr,
        const ApplicationUpdateSources& applicationSources = {}, const PolicyUpdateSources& policySources = {});
    ~MainWindow() override;
    void initializePolicyUpdates();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void configureToolbarIcons();
    void showReadOptionStatus(QString message);
    void showImageOptionProblem(QString message);
    void refreshLanguage();
    void showLanguages();
    void chooseLanguage(QAction* action);
    QMenu* languageMenu_ = nullptr;
    void createLogWindow();
    void refreshSelection();
    void finishImageSelection();
    void chooseFfuStore();
    void ffuStoreFinished(int result);
    void focusStart();
    void refreshElapsedTime();
    void updateElapsedTime();
    void chooseDevice(int index);
    void chooseImage();
    void downloadChoiceFinished(int result);
    void startDownload(const QString& path);
    void refreshDownload();
    void chooseBootMode(int index);
    void showLog();
    void toggleLog();
    void appendLog(const QString& message);
    void clearLog();
    void togglePersistentLog();
    void toggleSizeUnits();
    void resetUserSettings();
    void saveAutomaticLog();
    void showAbout();
    void showPolicyUpdates();
    void checkPolicies();
    void startPolicies(bool includeDbx);
    void policyProgress(QString message);
    void policyCompleted(rufus::PolicyUpdateResult result);
    bool updatesActive() const;
    void cancelUpdates();
    void updateFrequencyChanged(int interval);
    void applicationPreferenceChanged();
    void scheduleApplicationUpdates(int interval);
    void automaticApplicationCheck();
    void startApplicationUpdates(bool manual);
    void applicationCompleted(rufus::ApplicationUpdateResult result);
    void refreshUpdateStatus();
    void notifyApplicationUpdate();
    void applicationNoticeFinished(int result);
    void openReleasePage(const QUrl& page);
    void toggleForceUpdate();
    void toggleForceTimestampUpdate();
    void clearDownloadedResources();
    void toggleDarkAppearance();
    void toggleDeviceLocking();
    void increaseJobPriority();
    void decreaseJobPriority();
    void changeJobPriority(int delta);
    void setProgressFormat(const QString& format, const QString& phase, quint64 processed, quint64 total, bool active);
    ProgressDisplay progressDisplay_;
    QString progressFormat_;
    bool bareAlt_ = false, forceWindowsTimestamp_ = false;
    void appendLanguageNotice();
    void confirmDbxUpdate();
    void dbxUpdateFinished(int result);
    void initialUpdateChoice(int result);
    void schedulePolicyUpdates(int interval);
    void automaticPolicyCheck();
    void chooseLogFile();
    void saveLogFile(const QString& path);
    void chooseSaveImage();
    void chooseOpticalImage();
    void resetUsbPort();
    void toggleEspType();
    void refreshDeviceOperation();
    void saveDeviceImage(const QString& path);
    void updateSaveImageFilter(const QString& filter);
    void refreshSave();
    void toggleAdvancedDrive();
    void toggleAdvancedFormat();
    void toggleExpertMode();
    void toggleEnhancedInstall();
    void toggleWindowsMbr();
    void toggleFixedUsb();
    void showFixedUsbStatus(bool enabled);
    void confirmRevokedImage(int mask);
    void revocationWarningFinished(int result);
    void confirmWrite();
    void writeConfirmationFinished(int result);
    void confirmPartitions(bool raw);
    void partitionConfirmationFinished(int result);
    void confirmCancel();
    void cancelConfirmationFinished(int result);
    void refreshWrite();
    void refreshIsoWrite();
    void refreshCompletion();
    void refreshHashes();
    void showHashResult();
    void toggleExtraHashes();
    void toggleBootMarker();
    void zeroDevice();
    void fastZeroDevice();
    void toggleFakeDetection();
    void showBadBlocksReport(rufus::BadBlocksReport report, QString path);
    void badBlocksReviewFinished(int result);
    void chooseIsoMode();
    void isoModeFinished();
    void chooseWindowsOptions();
    void chooseWindowsArchive();
    void chooseWindowsEdition();
    void windowsImageFinished(int result);
    void chooseWindowsImageOption(int index);
    void chooseWindowsTarget(int index);
    void windowsOptionsFinished(int result);
    void chooseDosOptions();
    void chooseMsDosDownload();
    void msDosDownloadFinished(int result);
    void dosOptionsFinished(int result);
    void chooseGrubDownload(const QString& detectedVersion);
    void grubDownloadFinished(int result);
    void confirmIsoWrite();
    void isoConfirmationFinished(int result);
    void chooseCluster(int index);
    void refreshPersistence(bool enabled);
    void editPersistenceSize();
    void movePersistenceSlider(int value);
    void choosePersistenceUnits(int index);
    void chooseFileSystem(int index);
    void choosePartition(int index);

    SelectionController& controller_;
    std::unique_ptr<Ui::RufusWindow> ui_;
    QLabel* statusLabel_ = nullptr;
    QLabel* elapsedLabel_ = nullptr;
    QLabel* busyLabel_ = nullptr;
    QElapsedTimer elapsed_;
    QTimer elapsedTimer_;
    QMessageBox* imageNotice_ = nullptr;
    QMessageBox* badBlocksReview_ = nullptr;
    QDialog* logWindow_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QFileDialog* imageDialog_ = nullptr;
    ImageDownloadDialog* downloadDialog_=nullptr;
    QFileDialog* downloadFileDialog_=nullptr;
    QUrl downloadUrl_;
    QFileDialog* archiveDialog_ = nullptr;
    QFileDialog* saveLogDialog_ = nullptr;
    QFileDialog* saveImageDialog_ = nullptr;
    PolicyUpdates policyUpdates_;
    ApplicationUpdates applicationUpdates_;
    PolicyUpdateSources policySources_;
    QTimer applicationUpdateTimer_;
    QString applicationUpdateStatus_;
    ApplicationRelease applicationRelease_;
    QMessageBox* applicationUpdateNotice_ = nullptr;
    bool applicationCheckManual_ = false, forceUpdate_ = false;
    QString displayLanguage_;
    qint64 applicationRetryAfter_ = 0;
    QTimer policyTimer_;
    PolicyUpdateDialog* policyDialog_ = nullptr;
    QMessageBox *dbxUpdateDialog_ = nullptr, *initialUpdateDialog_ = nullptr;
    QString policyStatus_;
    bool checkingDbx_ = true;
    QMessageBox* revocationWarning_ = nullptr;
    QMessageBox* writeConfirmation_ = nullptr;
    QMessageBox* cancelConfirmation_ = nullptr;
    QMessageBox* isoConfirmation_ = nullptr;
    QMessageBox* partitionConfirmation_ = nullptr;
    bool partitionConfirmationRaw_ = false;
    ImageModeDialog* modeDialog_ = nullptr;
    WindowsUserDialog* windowsDialog_ = nullptr;
    WindowsImageDialog* windowsImageDialog_ = nullptr;
    FreeDosLocaleDialog* dosDialog_ = nullptr;
    QMessageBox* grubDownloadDialog_ = nullptr;
    bool creationCompleted_ = false;
    bool creationStarted_ = false;
    QString persistenceText_;
    ApplicationLog sessionLog_;
    bool persistentLog_ = false;
    bool properSizeUnits_ = false;
};

} // namespace rufus
