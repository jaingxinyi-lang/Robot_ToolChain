#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "script_panel.h"
#include "script_runner.h"
#include "process_watcher.h"
#include "logger.h"
#include "config_dialog.h"
#include "communication_manager.h"
#include "file_deployer.h"
#include "test_result_manager.h"
#include "mjpeg_stream_receiver.h"
#include "video_view.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDockWidget>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QHostAddress>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QList>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSizePolicy>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QCloseEvent>

namespace {

QString remoteScriptRoot()
{
    return QStringLiteral("/home/noetix/JIAOBEN");
}

QString normalizedRemoteScriptPath(const QString &scriptName)
{
    QString normalized = scriptName.trimmed();
    normalized.replace('\\', '/');
    if (normalized.isEmpty()) {
        return normalized;
    }

    const QString currentRoot = remoteScriptRoot();
    const QString currentPrefix = currentRoot + QStringLiteral("/");
    const QString legacyRoot = QStringLiteral("/JIAOBEN");
    const QString legacyPrefix = legacyRoot + QStringLiteral("/");

    if (normalized == currentRoot || normalized.startsWith(currentPrefix)) {
        return normalized;
    }

    if (normalized == legacyRoot) {
        return currentRoot;
    }
    if (normalized.startsWith(legacyPrefix)) {
        normalized = normalized.mid(legacyPrefix.size());
        return currentPrefix + normalized;
    }

    while (normalized.startsWith('/')) {
        normalized.remove(0, 1);
    }

    return normalized.isEmpty() ? currentRoot : currentPrefix + normalized;
}

bool usesLegacyRemoteScriptRoot(const QString &scriptPath)
{
    QString normalized = scriptPath.trimmed();
    normalized.replace('\\', '/');
    return normalized == QStringLiteral("/JIAOBEN")
        || normalized.startsWith(QStringLiteral("/JIAOBEN/"));
}

constexpr int kResourceSearchMaxDepth = 4;

// 开发态和发布态目录层级不同，资源文件统一从应用目录向上查找有限层级。
QString applicationRelativePath(const QString &relativePath)
{
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(relativePath);
}

QString findExistingPathInAncestors(const QString &relativePath,
                                    int maxDepth = kResourceSearchMaxDepth)
{
    QDir dir(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < maxDepth; ++depth) {
        const QString candidate = dir.absoluteFilePath(relativePath);
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
        dir.cdUp();
    }

    return QString();
}

QString resolveResourcePath(const QString &relativePath)
{
    const QString existingPath = findExistingPathInAncestors(relativePath);
    return existingPath.isEmpty() ? applicationRelativePath(relativePath) : existingPath;
}

struct ScriptUserFields {
    QString args;
    unsigned int timeoutMs { 0 };
};

struct ScriptUserFieldIndex {
    QHash<QString, ScriptUserFields> byPath;
    QSet<QString> duplicatePaths;
    QHash<QString, ScriptUserFields> byName;
    QHash<QString, int> oldNameCounts;
    QHash<QString, int> mappedNameCounts;
};

QString scriptPathKey(const QString &scriptPath)
{
    return normalizedRemoteScriptPath(scriptPath);
}

QString scriptNameKey(const QString &scriptName)
{
    return scriptName.trimmed().toCaseFolded();
}

void applyUserFields(ScriptConfig &config, const ScriptUserFields &fields)
{
    config.args = fields.args;
    config.timeoutMs = fields.timeoutMs;
}

ScriptUserFieldIndex buildScriptUserFieldIndex(
    const QVector<ScriptConfig> &currentConfigs,
    const QVector<TestResultManager::MappingEntry> &mappedEntries)
{
    ScriptUserFieldIndex index;

    for (const TestResultManager::MappingEntry &entry : mappedEntries) {
        if (entry.script.trimmed().isEmpty()) {
            continue;
        }

        const QString nameKey = scriptNameKey(entry.name);
        if (!nameKey.isEmpty()) {
            index.mappedNameCounts.insert(nameKey, index.mappedNameCounts.value(nameKey) + 1);
        }
    }

    for (const ScriptConfig &config : currentConfigs) {
        const ScriptUserFields fields{config.args, config.timeoutMs};
        const QString pathKey = scriptPathKey(config.path);
        if (!pathKey.isEmpty()) {
            if (index.byPath.contains(pathKey)) {
                index.duplicatePaths.insert(pathKey);
            } else {
                index.byPath.insert(pathKey, fields);
            }
        }

        const QString nameKey = scriptNameKey(config.name);
        if (!nameKey.isEmpty()) {
            index.oldNameCounts.insert(nameKey, index.oldNameCounts.value(nameKey) + 1);
            if (!index.byName.contains(nameKey)) {
                index.byName.insert(nameKey, fields);
            }
        }
    }

    return index;
}

void restoreUserFields(ScriptConfig &config, const ScriptUserFieldIndex &index)
{
    // 映射表只拥有名称和脚本路径；用户在配置框填写的参数/超时按脚本身份保留。
    const QString pathKey = scriptPathKey(config.path);
    if (!index.duplicatePaths.contains(pathKey) && index.byPath.contains(pathKey)) {
        applyUserFields(config, index.byPath.value(pathKey));
        return;
    }

    const QString nameKey = scriptNameKey(config.name);
    if (index.oldNameCounts.value(nameKey) == 1
        && index.mappedNameCounts.value(nameKey) == 1
        && index.byName.contains(nameKey)) {
        applyUserFields(config, index.byName.value(nameKey));
    }
}

constexpr const char *kVideoScriptFileName = "camera_opencv.sh";

bool isVideoScriptPath(const QString &scriptPath)
{
    QString normalized = scriptPath.trimmed();
    normalized.replace('\\', '/');
    if (normalized.isEmpty()) return false;
    const int slash = normalized.lastIndexOf('/');
    const QString fileName = (slash >= 0) ? normalized.mid(slash + 1) : normalized;
    return fileName.compare(QLatin1String(kVideoScriptFileName), Qt::CaseInsensitive) == 0;
}

void setupBusyDialog(QDialog &dialog,
                     const QString &windowTitle,
                     const QString &heading,
                     const QString &detail)
{
    dialog.setObjectName("BusyDialog");
    dialog.setWindowTitle(windowTitle);
    dialog.setModal(true);
    dialog.setFixedSize(560, 260);
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    dialog.setWindowFlag(Qt::MSWindowsFixedSizeDialogHint, true);
    dialog.setStyleSheet(
        "QDialog#BusyDialog { background: #f8fafc; }"
        "QFrame#AccentBar { background: #2563eb; border-radius: 2px; }"
        "QLabel#BusyTitle { color: #111827; font-size: 26px; font-weight: 700; }"
        "QLabel#BusyDetail { color: #4b5563; font-size: 17px; }"
        "QLabel#BusyBadge { background: #e0ecff; color: #1d4ed8; border: 1px solid #bfdbfe;"
        " border-radius: 12px; padding: 4px 10px; font-size: 15px; font-weight: 600; }");

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(30, 26, 30, 24);
    layout->setSpacing(14);

    auto *accentBar = new QFrame(&dialog);
    accentBar->setObjectName("AccentBar");
    accentBar->setFixedHeight(4);
    layout->addWidget(accentBar);

    auto *titleRow = new QHBoxLayout();
    titleRow->setSpacing(12);

    auto *titleLabel = new QLabel(heading, &dialog);
    titleLabel->setObjectName("BusyTitle");
    titleLabel->setWordWrap(true);
    titleRow->addWidget(titleLabel, 1);

    auto *badgeLabel = new QLabel("处理中", &dialog);
    badgeLabel->setObjectName("BusyBadge");
    badgeLabel->setAlignment(Qt::AlignCenter);
    badgeLabel->setFixedHeight(28);
    titleRow->addWidget(badgeLabel, 0, Qt::AlignTop);

    layout->addLayout(titleRow);

    auto *detailLabel = new QLabel(detail, &dialog);
    detailLabel->setObjectName("BusyDetail");
    detailLabel->setWordWrap(true);
    layout->addWidget(detailLabel);

    layout->addStretch();
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_scriptRunner(std::make_unique<ScriptRunner>())
    , m_processWatcher(std::make_unique<ProcessWatcher>())
    , m_processCheckMs(500)
    , m_currentPid(-1)
    , m_isScriptRunning(false)
    , m_currentSlotIndex(-1)
    , m_isRunAll(false)
    , m_timeoutTimer(new QTimer(this))
{
    ui->setupUi(this);
    configureResponsiveUi();
    setWindowTitle("布米调试工具 v1.0");
    setWindowIcon(QIcon(":/robot.png"));

    m_logger = std::make_unique<Logger>(ui->terminal_text);
    m_logger->setMaxLines(1000);
    m_logger->setTimestampEnabled(true);
    m_iperfProcess = std::make_unique<QProcess>();

    m_commDialog = std::make_unique<CommunicationDialog>(this);

        setupScriptDock();
        setupVideoDock();

    // 初始化脚本配置和加载持久化设置
    initializeScriptConfigs();  
    loadPersistentSettings();

    // 通信管理器
    m_commDialog->loadFromSettings();
    m_commDialog->getCommunicationConfig(m_commConfig);
    m_commManager = std::make_unique<CommunicationManager>(this);
    m_commManager->setConfig(m_commConfig);
    m_fileDeployer = std::make_unique<FileDeployer>(m_commManager.get(), this);
    m_testResultManager = std::make_unique<TestResultManager>(ui->groupBox, this);

    m_timeoutTimer->setSingleShot(true); 
    connect(m_timeoutTimer, &QTimer::timeout, this, &MainWindow::onExecutionTimeout);

    // 一键配置脚本间隔计时器
    m_runAllIntervalTimer = new QTimer(this);
    m_runAllIntervalTimer->setSingleShot(true);
    connect(m_runAllIntervalTimer, &QTimer::timeout, this, &MainWindow::onRunAllScriptIntervalElapsed);

    setupConnections();
    startIperfServer();
    loadResultMappings();
    // 初始化按钮状态：未连接通信
    updateButtonStates();

    m_logger->info("应用启动成功");
    m_logger->info(QString("已加载 %1 个脚本").arg(m_scriptConfigs.size()));
}

void MainWindow::setupScriptDock()
{
    m_scriptPanel = std::make_unique<ScriptPanel>(SCRIPT_SLOT_DEFAULT);
    m_scriptDock = new QDockWidget("", this);
    m_scriptDock->setWidget(m_scriptPanel.get());
    m_scriptDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_scriptDock->setVisible(false);
    addDockWidget(Qt::RightDockWidgetArea, m_scriptDock);
}

void MainWindow::setupVideoDock()
{
    m_videoView = new VideoView(this);
    m_videoDock = new QDockWidget(QStringLiteral("摄像头视频（10s）"), this);
    m_videoDock->setObjectName("VideoDock");
    m_videoDock->setWidget(m_videoView);
    m_videoDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_videoDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    m_videoDock->setVisible(false);
    addDockWidget(Qt::RightDockWidgetArea, m_videoDock);

    m_videoReceiver = std::make_unique<MjpegStreamReceiver>();
    connect(m_videoReceiver.get(), &MjpegStreamReceiver::frameReady,
            m_videoView, &VideoView::setFrame);
    connect(m_videoReceiver.get(), &MjpegStreamReceiver::streamStarted,
            this, &MainWindow::onVideoStreamStarted);
    connect(m_videoReceiver.get(), &MjpegStreamReceiver::streamError,
            this, &MainWindow::onVideoStreamError);
    connect(m_videoReceiver.get(), &MjpegStreamReceiver::streamStopped,
            this, &MainWindow::onVideoStreamStopped);
}

MainWindow::~MainWindow()
{
    stopIperfServer();
    if (m_videoReceiver) {
        m_videoReceiver->stop();
    }
    if (m_isScriptRunning) {
        m_scriptRunner->stopScript(true);
        m_isScriptRunning = false;
    }
    if (m_commManager && m_commManager->state() != CommunicationManager::State::Disconnected) {
        m_commManager->disconnectCurrent();
    }
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_isDeploying) {
        int ret = QMessageBox::warning(
            this,
            "确认关闭",
            "脚本包正在部署中，确实要关闭应用吗？\n部署将被取消。",
            QMessageBox::Yes | QMessageBox::No
        );
        if (ret == QMessageBox::No) {
            event->ignore();
            return;
        }
        m_fileDeployer->cancel();
        m_isDeploying = false;
    }

    if (m_isScriptRunning) {
        int ret = QMessageBox::warning(
            this,
            "确认关闭",
            "脚本正在运行中，确实要关闭应用吗？\n脚本将被强制停止。",
            QMessageBox::Yes | QMessageBox::No
        );
        if (ret == QMessageBox::No) {
            event->ignore();
            return;
        }
        m_scriptRunner->stopScript(true);
        // 标记为未运行，防止析构函数重复 stop
        m_isScriptRunning = false;
    }
    // 如果视频会话还在，接收端先收尾，避免阻塞关闭
    if (m_videoSessionActive) {
        stopVideoSession();
    }
    const bool communicationActive = m_commManager
        && m_commManager->state() != CommunicationManager::State::Disconnected;
    if (communicationActive && m_commManager->isConnected()) {
        if (!confirmAndCleanupDeployedPackage()) {
            event->ignore();
            return;
        }
    }

    const bool iperfActive = m_iperfProcess && m_iperfProcess->state() != QProcess::NotRunning;
    QDialog exitDialog(this);
    if (communicationActive || iperfActive) {
        setupBusyDialog(exitDialog,
                        "正在退出",
                        "正在退出，请稍候",
                        "正在断开通信连接并关闭后台服务");
        exitDialog.show();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    if (communicationActive) {
        m_commManager->disconnectCurrent();
    }
    stopIperfServer();
    event->accept();
}

void MainWindow::setupConnections()
{
    // ===== ScriptPanel 信号 =====
    connect(m_scriptPanel.get(), &ScriptPanel::scriptButtonClicked,
            this, &MainWindow::onScriptButtonClicked);

    // ===== 主窗口顶部按钮信号 =====
    connect(ui->connectBtn, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(ui->deployBtn, &QPushButton::clicked, this, &MainWindow::onDeployClicked);
    connect(ui->runAllBtn, &QPushButton::clicked, this, &MainWindow::onRunAllClicked);
    connect(ui->stopBtn,   &QPushButton::clicked, this, &MainWindow::onStopClicked);

    // ===== 菜单 Action 信号 =====
    connect(ui->actionSSHConfig,    &QAction::triggered, this, &MainWindow::onCommunicationConfigMenuTriggered);
    connect(ui->actionScriptConfig, &QAction::triggered, this, &MainWindow::onConfigMenuTriggered);
    connect(ui->actionScriptPanel,  &QAction::triggered, this, &MainWindow::onToggleScriptPanel);

    // ===== 通信管理器信号 =====
    connect(m_commManager.get(), &CommunicationManager::connected,
            this, &MainWindow::onCommunicationConnected);
    connect(m_commManager.get(), &CommunicationManager::disconnected,
            this, &MainWindow::onCommunicationDisconnected);
    connect(m_commManager.get(), &CommunicationManager::connectError,
            this, &MainWindow::onCommunicationConnectError);

    // ===== 部署器信号 =====
    connect(m_fileDeployer.get(), &FileDeployer::info,
            m_logger.get(), &Logger::info);
    connect(m_fileDeployer.get(), &FileDeployer::success,
            m_logger.get(), &Logger::success);
    connect(m_fileDeployer.get(), &FileDeployer::error,
            m_logger.get(), &Logger::error);
    connect(m_fileDeployer.get(), &FileDeployer::raw,
            m_logger.get(), &Logger::raw);

    connect(m_testResultManager.get(), &TestResultManager::unknownCodeReceived,
            this, &MainWindow::onUnknownResultCode);

    // ===== ScriptRunner 信号 =====
    connect(m_scriptRunner.get(), &ScriptRunner::outputReceived,
            this, &MainWindow::onScriptOutput);
    connect(m_scriptRunner.get(), &ScriptRunner::errorReceived,
            this, &MainWindow::onScriptError);
    connect(m_scriptRunner.get(), &ScriptRunner::started,
            this, &MainWindow::onScriptStarted);
    connect(m_scriptRunner.get(), &ScriptRunner::finished,
            this, &MainWindow::onScriptFinished);
    connect(m_scriptRunner.get(), &ScriptRunner::crashed,
            this, &MainWindow::onScriptCrashed);

    // ===== ProcessWatcher 信号 =====
    connect(m_processWatcher.get(), &ProcessWatcher::processExited,
            this, &MainWindow::onProcessExited);
    connect(m_processWatcher.get(), &ProcessWatcher::processError,
            this, &MainWindow::onProcessWatcherError);

    connect(m_iperfProcess.get(), &QProcess::readyReadStandardOutput, this, [this]() {
        const QString text = QString::fromUtf8(m_iperfProcess->readAllStandardOutput());
        if (!text.isEmpty()) {
            m_logger->raw(text);
        }
    });
    connect(m_iperfProcess.get(), &QProcess::readyReadStandardError, this, [this]() {
        const QString text = QString::fromUtf8(m_iperfProcess->readAllStandardError());
        if (!text.isEmpty()) {
            m_logger->raw(text);
        }
    });
    connect(m_iperfProcess.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (m_isStoppingIperf) {
            return;
        }
        if (error == QProcess::FailedToStart) {
            m_logger->error("iperf3 启动失败，请确认 iperf3.exe 与程序在同一目录");
        } else {
            m_logger->error(QString("iperf3 进程异常：%1").arg(m_iperfProcess->errorString()));
        }
    });
    connect(m_iperfProcess.get(), static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (m_isStoppingIperf) {
            return;
        }
        const QString statusText = exitStatus == QProcess::NormalExit ? "正常退出" : "异常退出";
        m_logger->error(QString("iperf3 监听已停止，状态：%1，退出码：%2").arg(statusText).arg(exitCode));
    });
}

// ── 菜单 ──────────────────────────────────────────────────────────────────────

void MainWindow::onToggleScriptPanel()
{
    const bool nowVisible = !m_scriptDock->isVisible();
    m_scriptDock->setVisible(nowVisible);
    if (nowVisible) {
        resizeDocks({m_scriptDock}, {static_cast<int>(width() * 0.15)}, Qt::Horizontal);
    }
}

void MainWindow::onConfigMenuTriggered()
{
    m_configDialog = std::make_unique<ConfigDialog>(m_scriptConfigs.size(), this);

    // 注入当前通信信息，供路径浏览功能使用
    m_configDialog->setCommunicationInfo(m_commManager.get());

    m_configDialog->setScriptConfigs(m_scriptConfigs);
    m_configDialog->setProcessCheckInterval(m_processCheckMs);

    if (m_configDialog->exec() == QDialog::Accepted) {
        m_configDialog->getScriptConfigs(m_scriptConfigs);
        m_processCheckMs = m_configDialog->getProcessCheckInterval();
        savePersistentSettings();

        m_scriptPanel->rebuildButtons(m_scriptConfigs.size());
        m_scriptPanel->updateButtonTexts(m_scriptConfigs);
        updateButtonStates();

        m_logger->success("配置已保存");
        for (int i = 0; i < m_scriptConfigs.size(); ++i) {
            const auto &cfg = m_scriptConfigs[i];
            if (!cfg.path.isEmpty()) {
                const QString displayName = cfg.name.isEmpty() ? QString("脚本 %1").arg(i + 1) : cfg.name;
                m_logger->info(QString("  [%1] %2: %3 参数=%4 (超时: %5ms)")
                    .arg(i)
                    .arg(displayName)
                    .arg(cfg.path)
                    .arg(cfg.args.isEmpty() ? QString("<空>") : cfg.args)
                    .arg(cfg.timeoutMs));
            }
        }
        m_logger->info(QString("进程检查间隔: %1 ms").arg(m_processCheckMs));
    }
}

void MainWindow::onCommunicationConfigMenuTriggered()
{
    if (m_commManager->isConnected() || m_isScriptRunning || m_isDeploying) {
        m_logger->error("请先停止任务并断开通信后再修改通信配置");
        return;
    }

    m_commDialog->setCommunicationConfig(m_commConfig);
    if (m_commDialog->exec() == QDialog::Accepted) {
        m_commDialog->getCommunicationConfig(m_commConfig);
        m_commManager->setConfig(m_commConfig);
        m_logger->success(QString("通信配置已保存：%1").arg(m_commManager->modeName()));
        updateButtonStates();
    }
}

// ── 脚本执行 ─────────────────────────────────────────────────────────────────

void MainWindow::onScriptButtonClicked(int slotIndex)
{
    // 第一层防护：运行时拒绝（按钮已被 setBusy 禁用，此处为兜底保护）
    if (m_isScriptRunning) {
        m_logger->error("脚本已在运行中，请勿重复点击");
        return;
    }
    if (m_testResultManager) {
        m_testResultManager->resetAll();
    }
    startScriptForSlot(slotIndex);
}

void MainWindow::onRunAllClicked()
{
    if (m_isScriptRunning) {
        m_logger->error("脚本已在运行中");
        return;
    }

    m_scriptQueue = runnableScriptSlots();

    if (m_scriptQueue.isEmpty()) {
        m_logger->error("一键配置：没有配置任何脚本路径");
        return;
    }

    m_logger->info(QString("=").repeated(60));
    m_logger->info(QString("一键配置开始，共 %1 个脚本").arg(m_scriptQueue.size()));
    if (m_testResultManager) {
        m_testResultManager->resetAll();
    }

    m_isRunAll = true;
    // 顶部按钮状态由 startScriptForSlot 内 setBusy 统一控制
    startNextInQueue();
}

void MainWindow::onStopClicked()
{
    if (m_isDeploying) {
        m_logger->error("用户取消脚本包部署");
        m_fileDeployer->cancel();
        return;
    }

    // 清空队列，阻止一键配置继续执行后续脚本
    m_scriptQueue.clear();
    m_isRunAll = false;
    m_runAllIntervalTimer->stop();

    if (m_isScriptRunning) {
        m_logger->error("用户强制停止脚本");
        m_timeoutTimer->stop();
        if (m_videoSessionActive) {
            stopVideoSession();
        }
        m_scriptRunner->stopScript(true);
        // 按钮状态将在 onScriptFinished/onScriptCrashed 中恢复
    } else {
        m_logger->info("无运行中的脚本");
        updateButtonStates();
    }
}

void MainWindow::startScriptForSlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= m_scriptConfigs.size()) {
        return;
    }

    const ScriptConfig &config = m_scriptConfigs[slotIndex];
    const QStringList args = config.args.isEmpty() ? QStringList() : QStringList{config.args};

    if (config.path.isEmpty()) {
        m_logger->error(QString("槽位 %1 (%2) 未配置脚本路径").arg(slotIndex).arg(config.name));
        // 一键配置模式下跳过空路径，继续下一个
        if (m_isRunAll) {
            startNextInQueue();
        }
        return;
    }

    m_logger->info(QString("=").repeated(60));
    m_logger->info(QString("执行脚本: [%1] %2").arg(slotIndex).arg(config.name));

    if (!m_scriptRunner->startScript(config.path, args)) {
        m_logger->error(QString("脚本启动失败: %1").arg(config.path));
        if (m_isRunAll) {
            m_scriptQueue.clear();
            m_isRunAll = false;
            updateButtonStates();
        }
        return;
    }

    m_isScriptRunning = true;
    m_currentSlotIndex = slotIndex;
    updateButtonStates();
    if (config.timeoutMs > 0) {
        m_timeoutTimer->start(config.timeoutMs);
    }
}

void MainWindow::startNextInQueue()
{
    // 一键配置的唯一出队点：队列为空表示整轮配置完成，否则启动下一个槽位。
    if (m_scriptQueue.isEmpty()) {
        m_isRunAll = false;
        m_logger->success("一键配置完成：所有脚本执行成功");
        m_logger->info(QString("=").repeated(60));
        updateButtonStates();
        return;
    }
    const int nextSlot = m_scriptQueue.dequeue();
    startScriptForSlot(nextSlot);
}

QQueue<int> MainWindow::runnableScriptSlots() const
{
    QQueue<int> runnableSlots;
    for (int i = 0; i < m_scriptConfigs.size(); ++i) {
        if (!m_scriptConfigs[i].path.isEmpty()) {
            runnableSlots.enqueue(i);
        }
    }
    return runnableSlots;
}

void MainWindow::resetScriptRunState()
{
    m_isScriptRunning = false;
    m_currentPid = -1;
    m_currentSlotIndex = -1;
}

// ── ScriptRunner 信号处理 ─────────────────────────────────────────────────────

void MainWindow::onScriptOutput(const QString &text)
{
    handleScriptOutputText(text);
}

void MainWindow::onScriptError(const QString &text)
{
    handleScriptOutputText(text);
}

void MainWindow::handleScriptOutputText(const QString &text)
{
    m_logger->raw(text);
    if (m_testResultManager) {
        m_testResultManager->applyOutputText(text);
    }

    // 摄像头脚本通过 stdout/stderr 上报握手字段，两路输出共用同一解析路径。
    if (m_videoSessionActive && !m_videoStreamReady) {
        const QStringList lines = text.split(QRegExp("[\\r\\n]"), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            parseVideoHandshakeLine(line);
            if (m_videoStreamReady) break;
        }
    }
}

void MainWindow::onScriptStarted(qint64 pid)
{
    m_currentPid = pid;
    if (pid > 0) {
        m_logger->success(QString("脚本执行开始，PID: %1").arg(pid));
        m_processWatcher->watchProcess(pid, m_processCheckMs);
    } else {
        m_logger->success("脚本执行开始（远程模式）");
        m_processWatcher->stopWatching();
    }

    beginVideoSessionIfNeeded();
}

void MainWindow::beginVideoSessionIfNeeded()
{
    if (m_currentSlotIndex >= 0 && m_currentSlotIndex < m_scriptConfigs.size()) {
        const ScriptConfig &cfg = m_scriptConfigs[m_currentSlotIndex];
        if (isVideoScriptPath(cfg.path)) {
            m_videoSessionActive = true;
            m_videoStreamReady   = false;
            m_videoBoardIp.clear();
            m_videoPath.clear();
            m_videoPort = 0;
            if (m_videoView) {
                m_videoView->clear();
                m_videoView->setPlaceholderText(QStringLiteral("正在启动摄像头推流…"));
            }
            m_logger->info(QStringLiteral("检测到摄像头会话，等待 VIDEO_READY"));
        }
    }
}

void MainWindow::onScriptFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_processWatcher->stopWatching();
    m_timeoutTimer->stop();
    if (m_videoSessionActive) {
        stopVideoSession();
    }

    const QString statusStr = (exitStatus == QProcess::NormalExit) ? "正常退出" : "异常退出";
    const bool    success   = (exitCode == 0 && exitStatus == QProcess::NormalExit);

    if (success) {
        m_logger->success(QString("脚本执行完成，状态: %1，退出码: %2").arg(statusStr).arg(exitCode));
    } else {
        m_logger->error(QString("脚本执行完成，状态: %1，退出码: %2").arg(statusStr).arg(exitCode));
    }
    m_logger->info(QString("=").repeated(60));

    resetScriptRunState();

    if (m_isRunAll) {
        if (success) {
            // 一键配置模式下，成功后启动 1s 间隔再执行下一个
            m_runAllIntervalTimer->start(1000);
        } else {
            m_scriptQueue.clear();
            m_isRunAll = false;
            m_logger->error("一键配置中止：脚本执行失败");
            updateButtonStates();
        }
    } else {
        updateButtonStates();
    }
}

void MainWindow::onScriptCrashed(const QString &errorMsg)
{
    m_timeoutTimer->stop();
    if (m_videoSessionActive) {
        stopVideoSession();
    }
    m_logger->error(errorMsg);
    resetScriptRunState();
    if (m_isRunAll) {
        m_scriptQueue.clear();
        m_isRunAll = false;
        m_logger->error("一键配置中止：脚本崩溃");
    }
    updateButtonStates();
}

// ── ProcessWatcher 信号处理 ───────────────────────────────────────────────────

void MainWindow::onProcessExited(qint64 pid, int)
{
    if (pid == m_currentPid && m_isScriptRunning) {
        m_logger->error(QString("进程意外退出，PID: %1").arg(pid));
        m_timeoutTimer->stop();
        resetScriptRunState();
        if (m_isRunAll) {
            m_scriptQueue.clear();
            m_isRunAll = false;
        }
        updateButtonStates();
    }
}

void MainWindow::onProcessWatcherError(const QString &errorMsg)
{
    m_logger->error(QString("进程监控异常: %1").arg(errorMsg));
}

void MainWindow::onExecutionTimeout()
{
    if (m_isScriptRunning) {
        m_logger->error("脚本执行超时，强制停止");
        // 先清队列，确保 onScriptFinished 不会继续执行下一个
        m_scriptQueue.clear();
        m_isRunAll = false;
        m_runAllIntervalTimer->stop();
        if (m_videoSessionActive) {
            stopVideoSession();
        }
        m_scriptRunner->stopScript(true);
    }
}

void MainWindow::onRunAllScriptIntervalElapsed()
{
    // 只有队列非空（还有后续脚本）时才打印继续执行日志
    if (!m_scriptQueue.isEmpty()) {
        m_logger->info("正在检索下一个脚本...");
    }
    startNextInQueue();
}

// ── 初始化 ────────────────────────────────────────────────────────────────────

void MainWindow::initializeScriptConfigs()
{
    m_processCheckMs = 500;
    m_scriptConfigs.clear();
    m_scriptConfigs.resize(SCRIPT_SLOT_DEFAULT);

    for (auto &config : m_scriptConfigs) {
        config.name.clear();
        config.path.clear();
        config.args.clear();
        config.timeoutMs = 0;
    }
    
    m_scriptPanel->rebuildButtons(m_scriptConfigs.size());
    m_scriptPanel->updateButtonTexts(m_scriptConfigs);
}

void MainWindow::loadPersistentSettings()
{
    QSettings settings("RobotTeam", "RobotToolChain");

    settings.beginGroup("Scripts");
    const int fallbackSlotCount = qMax(m_scriptConfigs.size(), settings.childGroups().size());
    const int slotCount = qBound(SCRIPT_SLOT_MIN,
                                 settings.value("slotCount", fallbackSlotCount).toInt(),
                                 SCRIPT_SLOT_MAX);
    m_scriptConfigs.resize(slotCount);
    bool migratedScriptPaths = false;

    for (int i = 0; i < m_scriptConfigs.size(); ++i) {
        const QString slotGroup = QString("Slot%1").arg(i);
        settings.beginGroup(slotGroup);

        if (settings.contains("name")) {
            m_scriptConfigs[i].name = settings.value("name").toString();
        }
        if (settings.contains("path")) {
            const QString storedPath = settings.value("path").toString();
            if (usesLegacyRemoteScriptRoot(storedPath)) {
                m_scriptConfigs[i].path = normalizedRemoteScriptPath(storedPath);
                migratedScriptPaths = true;
            } else {
                m_scriptConfigs[i].path = storedPath;
            }
        }
        if (settings.contains("args")) {
            m_scriptConfigs[i].args = settings.value("args").toString();
        }
        if (settings.contains("timeoutMs")) {
            m_scriptConfigs[i].timeoutMs = settings.value("timeoutMs").toUInt();
        }

        settings.endGroup();
    }
    settings.endGroup();

    settings.beginGroup("Runtime");
    if (settings.contains("processCheckMs")) {
        m_processCheckMs = settings.value("processCheckMs").toUInt();
    }
    settings.endGroup();

    if (migratedScriptPaths) {
        savePersistentSettings();
    }

    m_scriptPanel->rebuildButtons(m_scriptConfigs.size());
    m_scriptPanel->updateButtonTexts(m_scriptConfigs);
}

void MainWindow::savePersistentSettings() const
{
    QSettings settings("RobotTeam", "RobotToolChain");

    settings.beginGroup("Scripts");
    settings.remove("");
    settings.setValue("slotCount", m_scriptConfigs.size());
    for (int i = 0; i < m_scriptConfigs.size(); ++i) {
        const QString slotGroup = QString("Slot%1").arg(i);
        settings.beginGroup(slotGroup);
        settings.setValue("name", m_scriptConfigs[i].name);
        settings.setValue("path", m_scriptConfigs[i].path);
        settings.setValue("args", m_scriptConfigs[i].args);
        settings.setValue("timeoutMs", m_scriptConfigs[i].timeoutMs);
        settings.endGroup();
    }
    settings.endGroup();

    settings.beginGroup("Runtime");
    settings.setValue("processCheckMs", m_processCheckMs);
    settings.endGroup();
    settings.sync();
}

void MainWindow::configureResponsiveUi()
{
    ui->menubar->setStyleSheet(
        "QMenuBar { background: #f6f8fb; color: #1f2933; font-size: 12pt;"
        " border-bottom: 1px solid #cbd5e1; }"
        "QMenuBar::item { background: transparent; padding: 5px 12px; margin: 1px 2px;"
        " border-radius: 4px; }"
        "QMenuBar::item:selected:enabled { background: #dbeafe; color: #0f172a; }"
        "QMenuBar::item:pressed:enabled { background: #bfdbfe; color: #0f172a; }"
        "QMenuBar::item:disabled { background: transparent; color: #94a3b8; }"
        "QMenu { background: #ffffff; color: #1f2933; font-size: 12pt;"
        " border: 1px solid #cbd5e1; }"
        "QMenu::item { background: transparent; padding: 7px 30px 7px 18px; }"
        "QMenu::item:selected:enabled { background: #dbeafe; color: #0f172a; }"
        "QMenu::item:disabled { background: transparent; color: #94a3b8; }"
        "QMenu::item:selected:disabled { background: transparent; color: #94a3b8; }");

    ui->terminal_text->setAcceptRichText(true);
    ui->terminal_text->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    const QList<QPushButton *> topButtons = {
        ui->connectBtn,
        ui->deployBtn,
        ui->runAllBtn,
        ui->stopBtn
    };
    for (QPushButton *button : topButtons) {
        button->setMinimumHeight(58);
        button->setMaximumHeight(92);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }

    ui->groupBox->setMinimumHeight(230);
    ui->groupBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

// ── 通信控制 ──────────────────────────────────────────────────────────────────

void MainWindow::onConnectClicked()
{
    if (m_commManager->isConnected()) {
        // 已连接 → 断开
        if (m_isScriptRunning || m_isDeploying) {
            m_logger->error("请先停止当前任务再断开通信连接");
            return;
        }
        if (!confirmAndCleanupDeployedPackage()) {
            return;
        }
        m_commManager->disconnectCurrent();
    } else if (m_commManager->state() == CommunicationManager::State::Disconnected) {
        // 未连接 → 连接
        const CommunicationConfig config = m_commManager->config();
        if (config.mode == CommunicationMode::Serial) {
            m_logger->info(QString("正在连接串口：%1 @ %2 ...")
                .arg(config.serial.portName)
                .arg(config.serial.baudRate));
        } else {
            m_logger->info(QString("正在连接 SSH：%1@%2:%3 ...")
                .arg(config.ssh.user, config.ssh.host).arg(config.ssh.port));
        }
        updateConnectButton();   // 立即变"连接中..."
        m_commManager->connectCurrent();
    }
}

void MainWindow::onCommunicationConnected()
{
    m_scriptRunner->setRemoteManager(m_commManager.get());
    if (m_testResultManager) {
        m_testResultManager->resetAll();
    }
    m_logger->success(QString("%1 连接成功").arg(m_commManager->modeName()));
    updateButtonStates();
}

void MainWindow::onCommunicationDisconnected(const QString &reason)
{
    // 若脚本正在运行，先强停
    if (m_isScriptRunning) {
        m_timeoutTimer->stop();
        m_scriptQueue.clear();
        m_isRunAll = false;
        m_runAllIntervalTimer->stop();
        if (m_videoSessionActive) {
            stopVideoSession();
        }
        m_scriptRunner->stopScript(true);
        resetScriptRunState();
        m_logger->error("通信断开导致脚本被强制停止");
    }
    m_scriptRunner->clearRemoteMode();
    m_logger->error(QString("%1 已断开：%2").arg(m_commManager->modeName(), reason));
    updateButtonStates();
}

void MainWindow::onCommunicationConnectError(const QString &message)
{
    m_logger->error(QString("%1").arg(message));
    updateButtonStates();
}

void MainWindow::onDeployClicked()
{
    if (!m_commManager->isConnected()) {
        m_logger->error("请先建立通信连接，再部署脚本包");
        return;
    }
    if (m_isScriptRunning || m_isDeploying) {
        m_logger->error("当前有任务正在运行，请稍后再部署脚本包");
        return;
    }

    m_isDeploying = true;
    updateButtonStates();
    const bool success = m_fileDeployer->deployPackage(packagePath());
    m_isDeploying = false;
    if (success) {
        configureScriptsFromMapping();
    }
    if (!success && !m_fileDeployer->isCancelled()) {
        m_logger->error("脚本包部署失败");
    }
    updateButtonStates();
}

void MainWindow::onUnknownResultCode(const QString &code)
{
    m_logger->info(QString("收到未配置的测试结果码：%1").arg(code));
}

// ── 按钮状态统一管理 ──────────────────────────────────────────────────────────

void MainWindow::updateButtonStates()
{
    const bool connected = m_commManager->isConnected();
    const bool connecting = (m_commManager->state() == CommunicationManager::State::Connecting);
    const bool busy       = m_isScriptRunning || m_isDeploying;

    // connectBtn
    updateConnectButton();

    // 只有通信已连接且不在跑脚本/部署时，才能点"一键配置"
    ui->runAllBtn->setEnabled(connected && !busy);
    ui->deployBtn->setEnabled(connected && !busy);
    // stop 在跑脚本或部署时可用
    ui->stopBtn->setEnabled(busy);
    // 脚本面板按钮
    m_scriptPanel->setBusy(!connected || busy);
    // 菜单："脚本配置..." 需要通信已连接
    ui->actionScriptConfig->setEnabled(connected && !busy);
    // "通信配置..." 仅在未连接且空闲时可修改
    ui->actionSSHConfig->setEnabled(!connected && !connecting && !busy);

}

void MainWindow::updateConnectButton()
{
    const auto state = m_commManager->state();
    if (state == CommunicationManager::State::Connected) {
        ui->connectBtn->setEnabled(!m_isDeploying && !m_isScriptRunning);
        ui->connectBtn->setText(m_commManager->connectedActionText());
        ui->connectBtn->setStyleSheet(
            "QPushButton { background-color: #2980b9; color: white; border-radius: 6px;"
            " font-size: 28px; font-weight: bold; }"
            "QPushButton:hover { background-color: #3498db; }"
            "QPushButton:pressed { background-color: #1f618d; }"
            "QPushButton:disabled { background-color: #4a4a4a; color: #888888; }");
    } else if (state == CommunicationManager::State::Connecting) {
        ui->connectBtn->setEnabled(false);
        ui->connectBtn->setText("连接中...");
        ui->connectBtn->setStyleSheet(
            "QPushButton { background-color: #f39c12; color: white; border-radius: 6px;"
            " font-size: 28px; font-weight: bold; }"
            "QPushButton:disabled { background-color: #f39c12; color: white; }");
    } else {
        ui->connectBtn->setEnabled(true);
        ui->connectBtn->setText(m_commManager->disconnectedActionText());
        ui->connectBtn->setStyleSheet(
            "QPushButton { background-color: #2ecc71; color: white; border-radius: 6px;"
            " font-size: 28px; font-weight: bold; }"
            "QPushButton:hover { background-color: #27ae60; }"
            "QPushButton:pressed { background-color: #1e8449; }"
            "QPushButton:disabled { background-color: #4a4a4a; color: #888888; }");
    }
}

QString MainWindow::iperfPath() const
{
    const QString sameDirExe = applicationRelativePath("iperf3.exe");
    if (QFileInfo::exists(sameDirExe)) {
        return sameDirExe;
    }

    const QString clientExe = findExistingPathInAncestors("client/iperf3.exe");
    return clientExe.isEmpty() ? sameDirExe : clientExe;
}

void MainWindow::startIperfServer()
{
    if (!m_iperfProcess || m_iperfProcess->state() != QProcess::NotRunning) {
        return;
    }

    const QString exePath = iperfPath();
    if (!QFileInfo::exists(exePath)) {
        m_logger->error(QString("未找到 iperf3.exe：%1").arg(exePath));
        return;
    }

    m_isStoppingIperf = false;
    m_iperfProcess->setProgram(exePath);
    m_iperfProcess->setArguments({"-s"});
    m_iperfProcess->setWorkingDirectory(QFileInfo(exePath).absolutePath());
    m_iperfProcess->start();
    if (m_iperfProcess->waitForStarted(3000)) {
        m_logger->success(QString("iperf3 已启动监听：%1 -s").arg(exePath));
    } else {
        m_logger->error(QString("iperf3 启动失败：%1").arg(m_iperfProcess->errorString()));
    }
}

void MainWindow::stopIperfServer()
{
    if (!m_iperfProcess || m_iperfProcess->state() == QProcess::NotRunning) {
        return;
    }

    m_isStoppingIperf = true;
    m_logger->info("正在关闭 iperf3 监听进程...");
    m_iperfProcess->terminate();
    if (!m_iperfProcess->waitForFinished(700)) {
        m_iperfProcess->kill();
        m_iperfProcess->waitForFinished(300);
    }
    m_logger->success("iperf3 监听进程已关闭");
    m_isStoppingIperf = false;
}

QString MainWindow::packagePath() const
{
    return resolveResourcePath("zip/JIAOBEN.zip");
}

QString MainWindow::mappingPath() const
{
    return resolveResourcePath("mapping/test_path.csv");
}

void MainWindow::loadResultMappings()
{
    if (!m_testResultManager) {
        return;
    }

    const QString selectedPath = mappingPath();

    if (!QFileInfo::exists(selectedPath)) {
        m_logger->info(QString("未找到映射表：%1，执行结果卡片暂不显示项目").arg(selectedPath));
        return;
    }

    QString errorMessage;
    if (m_testResultManager->loadFromCsv(selectedPath, &errorMessage)) {
        m_logger->success(QString("已加载测试项：%1 个项目").arg(m_testResultManager->cardCount()));
    } else {
        m_logger->error(errorMessage);
    }
}

void MainWindow::configureScriptsFromMapping()
{
    if (!m_testResultManager) {
        m_logger->error("部署成功，但结果管理器未初始化，无法自动配置脚本列表");
        return;
    }

    if (m_testResultManager->entries().isEmpty()) {
        loadResultMappings();
    }

    const QVector<TestResultManager::MappingEntry> &entries = m_testResultManager->entries();
    if (entries.isEmpty()) {
        m_logger->error("部署成功，但未加载到映射表，无法自动配置脚本列表");
        return;
    }

    // 部署后脚本槽位会按映射表重建；先建立旧用户字段索引，避免参数被重建流程清空。
    const ScriptUserFieldIndex userFieldIndex = buildScriptUserFieldIndex(m_scriptConfigs, entries);

    m_scriptConfigs.clear();
    m_scriptConfigs.reserve(entries.size());
    for (const TestResultManager::MappingEntry &entry : entries) {
        if (entry.script.trimmed().isEmpty()) {
            continue;
        }

        ScriptConfig config;
        config.name = entry.name;
        config.path = remoteScriptPath(entry.script);
        restoreUserFields(config, userFieldIndex);
        m_scriptConfigs.push_back(config);
    }

    if (m_scriptConfigs.isEmpty()) {
        m_logger->error("部署成功，但映射表中的测试脚本列为空");
        return;
    }

    savePersistentSettings();
    m_scriptPanel->rebuildButtons(m_scriptConfigs.size());
    m_scriptPanel->updateButtonTexts(m_scriptConfigs);
    updateButtonStates();
    m_logger->success(QString("已根据映射表自动配置 %1 个脚本，目录前缀：%2/")
        .arg(m_scriptConfigs.size())
        .arg(remoteScriptRoot()));
}

QString MainWindow::remoteScriptPath(const QString &scriptName) const
{
    return normalizedRemoteScriptPath(scriptName);
}

bool MainWindow::confirmAndCleanupDeployedPackage()
{
    if (!m_commManager || !m_commManager->isConnected()) {
        return true;
    }

    const QMessageBox::StandardButton decision = QMessageBox::question(
        this,
        "断开通信",
        QString("断开通信前是否删除开发板上的部署脚本包？\n\n将删除：%1 以及临时上传文件。")
            .arg(remoteScriptRoot()),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
        QMessageBox::Yes);

    if (decision == QMessageBox::Cancel) {
        return false;
    }
    if (decision == QMessageBox::No) {
        return true;
    }

    m_logger->info("正在删除开发板上的部署脚本包...");
    QDialog cleanupDialog(this);
    setupBusyDialog(cleanupDialog,
                    "正在清理",
                    "正在删除部署脚本包",
                    QString("正在清理开发板上的 %1 和临时上传文件。")
                        .arg(remoteScriptRoot()));
    cleanupDialog.show();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    QString output;
    QString errorOutput;
    int exitCode = -1;
    const QString cleanupCommand = QString("rm -rf %1 /tmp/JIAOBEN.zip /tmp/JIAOBEN.zip.b64 && sync")
        .arg(remoteScriptRoot());
    const bool ok = m_commManager->executeCommand(cleanupCommand, output, &errorOutput, &exitCode, 12000);
    cleanupDialog.close();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if (ok && exitCode == 0) {
        m_logger->success("已删除开发板上的部署脚本包");
        return true;
    }

    const QString details = !errorOutput.trimmed().isEmpty()
        ? errorOutput.trimmed()
        : output.trimmed();
    m_logger->error(details.isEmpty()
        ? QString("部署脚本包删除失败，退出码：%1").arg(exitCode)
        : QString("部署脚本包删除失败：%1").arg(details));

    return QMessageBox::warning(
        this,
        "删除失败",
        "部署脚本包删除失败，是否仍然断开通信？",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No) == QMessageBox::Yes;
}

// ── 摄像头视频会话 ───────────────────────────────────────────────────────────

void MainWindow::parseVideoHandshakeLine(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty()) return;

    if (trimmed.startsWith(QStringLiteral("BOARD_VIDEO_PORT="))) {
        const QString val = trimmed.mid(QStringLiteral("BOARD_VIDEO_PORT=").size()).trimmed();
        bool ok = false;
        const int p = val.toInt(&ok);
        if (ok && p > 0 && p <= 65535) {
            m_videoPort = p;
        }
        return;
    }
    if (trimmed.startsWith(QStringLiteral("BOARD_VIDEO_PATH="))) {
        m_videoPath = trimmed.mid(QStringLiteral("BOARD_VIDEO_PATH=").size()).trimmed();
        return;
    }
    if (trimmed.startsWith(QStringLiteral("BOARD_IP="))) {
        m_videoBoardIp = trimmed.mid(QStringLiteral("BOARD_IP=").size()).trimmed();
        return;
    }
    if (trimmed == QStringLiteral("VIDEO_READY")) {
        tryStartVideoStream();
        return;
    }
}

void MainWindow::tryStartVideoStream()
{
    if (m_videoStreamReady) return;
    if (m_videoPort <= 0 || m_videoPath.isEmpty()) {
        m_logger->error(QStringLiteral("收到 VIDEO_READY 但握手字段不全，放弃视频显示"));
        return;
    }

    // SSH 模式优先用主机名/IP；串口模式用脚本上报的 BOARD_IP。
    QString host;
    if (m_commConfig.mode == CommunicationMode::Ssh) {
        host = m_commConfig.ssh.host.trimmed();
    }
    if (host.isEmpty()) {
        host = m_videoBoardIp.trimmed();
    }
    if (host.isEmpty() || host == QLatin1String("0.0.0.0")) {
        m_logger->error(QStringLiteral("无法确定板卡 IP，无法显示视频"));
        return;
    }

    QString path = m_videoPath;
    if (!path.startsWith('/')) path.prepend('/');

    QUrl url;
    url.setScheme("http");
    url.setHost(host);
    url.setPort(m_videoPort);
    // setPath 不接受查询串，需用 setUrl 完整解析。
    const QString full = QStringLiteral("http://%1:%2%3").arg(host).arg(m_videoPort).arg(path);
    url = QUrl(full);
    if (!url.isValid()) {
        m_logger->error(QStringLiteral("视频地址无效: %1").arg(full));
        return;
    }

    m_videoStreamReady = true;
    if (m_videoView) {
        m_videoView->setPlaceholderText(QStringLiteral("正在接收视频…"));
    }
    if (m_videoDock) {
        m_videoDock->setVisible(true);
        m_videoDock->raise();
    }
    m_logger->info(QStringLiteral("启动视频接收: %1").arg(url.toString()));
    if (m_videoReceiver) {
        m_videoReceiver->start(url);
    }
}

void MainWindow::stopVideoSession()
{
    if (!m_videoSessionActive && !m_videoStreamReady) {
        if (m_videoDock) m_videoDock->setVisible(false);
        return;
    }
    if (m_videoReceiver) {
        m_videoReceiver->stop();
    }
    if (m_videoView) {
        m_videoView->clear();
        m_videoView->setPlaceholderText(QStringLiteral("等待视频信号..."));
    }
    if (m_videoDock) {
        m_videoDock->setVisible(false);
    }
    m_videoSessionActive = false;
    m_videoStreamReady   = false;
    m_videoBoardIp.clear();
    m_videoPath.clear();
    m_videoPort = 0;
}

void MainWindow::onVideoFrameReady(const QImage & /*image*/)
{
    // VideoView 已通过 frameReady 信号直接更新，这里保留槽位以便未来扩展。
}

void MainWindow::onVideoStreamStarted()
{
    m_logger->success(QStringLiteral("视频流已建立"));
}

void MainWindow::onVideoStreamError(const QString &message)
{
    m_logger->error(QStringLiteral("视频流错误: %1").arg(message));
}

void MainWindow::onVideoStreamStopped()
{
    // 仅日志；UI 收尾在 stopVideoSession 中完成。
}
