// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QMainWindow>
#include <memory>

class QCloseEvent;
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

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(SelectionController& controller, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void configureToolbarIcons();
    void createLogWindow();
    void refreshSelection();
    void chooseDevice(int index);
    void chooseImage();
    void showLog();
    void chooseLogFile();
    void saveLogFile(const QString& path);
    void toggleAdvancedDrive();
    void toggleAdvancedFormat();
    void confirmWrite();
    void writeConfirmationFinished(int result);
    void confirmCancel();
    void cancelConfirmationFinished(int result);
    void refreshWrite();

    SelectionController& controller_;
    std::unique_ptr<Ui::RufusWindow> ui_;
    QLabel* statusLabel_ = nullptr;
    QDialog* logWindow_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QFileDialog* imageDialog_ = nullptr;
    QFileDialog* saveLogDialog_ = nullptr;
    QMessageBox* writeConfirmation_ = nullptr;
    QMessageBox* cancelConfirmation_ = nullptr;
};

} // namespace rufus
