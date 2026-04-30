#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QProcess>
#include <QQueue>
#include <QTimer>
#include <QVector>
#include <memory>
#include "config_dialog.h"  // ScriptConfig, CommunicationConfig, SCRIPT_SLOT_* 常量

class ScriptRunner;
class ProcessWatcher;
class Logger;
class ConfigDialog;
class ScriptPanel;
class CommunicationManager;
class FileDeployer;
class TestResultManager;
class MjpegStreamReceiver;
class VideoView;
class QDockWidget;
class QImage;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

/**
 * @class MainWindow
 * @brief 主窗口 - UI 展示与业务流程协调器
 *
 * MainWindow 持有主要界面对象，并负责把用户操作串联到通信、部署、
 * 脚本执行、结果卡片和视频流等专用组件。底层 I/O 与执行细节仍由
 * CommunicationManager、FileDeployer、ScriptRunner 等类承担。
 *
 * 核心流程：
 * - 单脚本执行：onScriptButtonClicked -> startScriptForSlot -> ScriptRunner
 *   -> onScriptOutput/onScriptError -> onScriptFinished/onScriptCrashed。
 * - 一键配置：onRunAllClicked 初始化队列，startNextInQueue 逐个出队，
 *   每个脚本成功结束后由 m_runAllIntervalTimer 延迟 1 秒继续下一个。
 * - 脚本包部署：onDeployClicked -> FileDeployer -> configureScriptsFromMapping，
 *   部署后按映射表刷新脚本名称/路径，并保留用户配置的参数和超时。
 * - 视频会话：摄像头脚本启动后解析脚本输出中的握手字段，收到
 *   VIDEO_READY 后启动 MJPEG 接收。
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    // ===== ScriptPanel 转发信号 =====
    void onScriptButtonClicked(int slotIndex);
    void onRunAllClicked();
    void onStopClicked();
    void onToggleScriptPanel();

    // ===== 菜单信号 =====
    void onConfigMenuTriggered();

    // ===== ScriptRunner 信号 =====
    void onScriptOutput(const QString &text);
    void onScriptError(const QString &text);
    void onScriptStarted(qint64 pid);
    void onScriptFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onScriptCrashed(const QString &errorMsg);

    // ===== ProcessWatcher 信号 =====
    void onProcessExited(qint64 pid, int exitCode);
    void onProcessWatcherError(const QString &errorMsg);

    // ===== 超时处理 =====
    void onExecutionTimeout();

    // ===== 一键配置脚本间隔 =====
    void onRunAllScriptIntervalElapsed();

    // ===== 通信控制 =====
    void onConnectClicked();
    void onCommunicationConnected();
    void onCommunicationDisconnected(const QString &reason);
    void onCommunicationConnectError(const QString &message);
    void onCommunicationConfigMenuTriggered();
    void onDeployClicked();
    void onUnknownResultCode(const QString &code);

    // ===== 视频流信号 =====
    void onVideoFrameReady(const QImage &image);
    void onVideoStreamStarted();
    void onVideoStreamError(const QString &message);
    void onVideoStreamStopped();

private:
    // UI 组装：构造函数只负责串联步骤，具体控件初始化下沉到这些函数。
    void setupScriptDock();
    void setupVideoDock();

    // 摄像头脚本视频会话：脚本输出握手字段，MainWindow 负责聚合并启动接收器。
    void parseVideoHandshakeLine(const QString &line);
    void tryStartVideoStream();
    void stopVideoSession();
    void beginVideoSessionIfNeeded();

    // 脚本 stdout/stderr 共用处理：日志、结果码识别、视频握手解析。
    void handleScriptOutputText(const QString &text);

    void setupConnections();
    void initializeScriptConfigs();
    void loadPersistentSettings();
    void savePersistentSettings() const;
    void configureResponsiveUi();

    // 启动指定槽位的脚本（内部使用，不检查 m_isScriptRunning）
    void startScriptForSlot(int slotIndex);
    // 从队列取出下一个槽位执行；队列为空时结束一键配置
    void startNextInQueue();
    // 生成一键配置队列；仅包含已配置路径的脚本槽位。
    QQueue<int> runnableScriptSlots() const;
    // 复位当前脚本运行态；不处理队列、计时器或外部进程终止。
    void resetScriptRunState();
    // 统一更新所有按钮状态
    void updateButtonStates();
    // 不同连接状态下修改 connectBtn 文字和样式
    void updateConnectButton();
    QString packagePath() const;
    QString mappingPath() const;
    QString iperfPath() const;
    void startIperfServer();
    void stopIperfServer();
    void loadResultMappings();
    void configureScriptsFromMapping();
    QString remoteScriptPath(const QString &scriptName) const;
    bool confirmAndCleanupDeployedPackage();

    // UI 指针
    Ui::MainWindow *ui;

    // 核心业务逻辑类
    std::unique_ptr<ScriptRunner>   m_scriptRunner;
    std::unique_ptr<ProcessWatcher>  m_processWatcher;
    std::unique_ptr<Logger>          m_logger;
    std::unique_ptr<ConfigDialog>    m_configDialog;
    std::unique_ptr<CommunicationDialog> m_commDialog;
    std::unique_ptr<ScriptPanel>     m_scriptPanel;
    std::unique_ptr<CommunicationManager> m_commManager;
    std::unique_ptr<FileDeployer>    m_fileDeployer;
    std::unique_ptr<TestResultManager> m_testResultManager;
    std::unique_ptr<QProcess>        m_iperfProcess;
    std::unique_ptr<MjpegStreamReceiver> m_videoReceiver;
    QDockWidget                     *m_scriptDock{nullptr};
    QDockWidget                     *m_videoDock{nullptr};
    VideoView                       *m_videoView{nullptr};

    // 视频会话状态
    bool    m_videoSessionActive{false};
    bool    m_videoStreamReady{false};
    QString m_videoBoardIp;
    int     m_videoPort{0};
    QString m_videoPath;

    // 脚本配置
    QVector<ScriptConfig> m_scriptConfigs;
    CommunicationConfig m_commConfig;

    // 配置参数
    unsigned int m_processCheckMs;

    // 状态标志
    qint64 m_currentPid;
    bool   m_isScriptRunning;
    bool   m_isDeploying{false};
    bool   m_isStoppingIperf{false};
    int    m_currentSlotIndex;
    bool   m_isRunAll;

    // 一键配置执行队列（存放待执行的槽位索引）
    QQueue<int> m_scriptQueue;

    // 超时计时器（复用单一实例，避免重复创建）
    QTimer *m_timeoutTimer;
    // 一键配置脚本间隔计时器（1秒间隔）
    QTimer *m_runAllIntervalTimer{nullptr};
};

#endif // MAINWINDOW_H
