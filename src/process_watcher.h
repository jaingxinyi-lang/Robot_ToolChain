#ifndef PROCESS_WATCHER_H
#define PROCESS_WATCHER_H

#include <QObject>
#include <QTimer>

/**
 * @class ProcessWatcher
 * @brief 进程监控器 - 持续监控指定PID的进程生命周期
 *
 * 功能特性:
 * - 定时检查进程是否仍然存活
 * - 检测进程异常退出并发出信号
 * - 作为ScriptRunner的补充防护，防止进程状态漂移
 * - 支持实时的健康检查回调
 *
 * 使用场景:
 * - ScriptRunner意外崩溃导致状态不同步时的备用检测
 * - UI需要定期更新进程状态时的数据源
 *
 * 使用示例:
 * @code
 *   ProcessWatcher watcher;
 *   connect(&watcher, &ProcessWatcher::processExited, this, [](qint64 pid) {
 *       qInfo() << "Process exited:" << pid;
 *   });
 *   watcher.watchProcess(12345, 500);  // 每500ms检查一次PID 12345
 * @endcode
 */
class ProcessWatcher : public QObject {
    Q_OBJECT

public:
    /**
     * 构造函数
     * @param parent: 父对象
     */
    explicit ProcessWatcher(QObject *parent = nullptr);

    /**
     * 析构函数
     */
    ~ProcessWatcher();

    /**
     * 开始监控指定PID的进程
     *
     * @param pid: 要监控的进程ID
     * @param checkIntervalMs: 检查间隔（毫秒），推荐500-1000ms
     * @return: 成功开始监控返回true，失败返回false
     *
     * @note: 如果已有监控任务正在运行，会先停止旧的任务
     * @note: 检查间隔过短（<100ms）可能浪费资源；过长（>5000ms）可能延迟检测
     * @warning: pid必须是有效的正整数
     *
     * @example:
     * @code
     *   watcher.watchProcess(12345, 500);  // 每500ms检查是否存活
     * @endcode
     */
    bool watchProcess(qint64 pid, unsigned int checkIntervalMs = 500);

    /**
     * 停止进程监控
     *
     * @note: 停止后需要调用watchProcess()重新启动监控
     */
    void stopWatching();

    /**
     * 检查进程是否仍然存活
     *
     * @param pid: 要检查的进程ID
     * @return: true=进程存活，false=进程已结束或PID无效
     *
     * @note: 该函数执行同步检查，立即返回结果
    * @note: Windows 使用系统进程句柄检查，其他平台访问 /proc/[pid]/stat。
     */
    bool isProcessAlive(qint64 pid) const;

    /**
     * 获取当前被监控的进程ID
     *
     * @return: 监控中的进程ID，-1表示未在监控任何进程
     */
    qint64 getWatchedPid() const;

    /**
     * 检查是否正在监控中
     *
     * @return: true=正在监控，false=未监控或已停止
     */
    bool isWatching() const;

signals:
    /**
     * 被监控进程已结束信号
     *
     * @param pid: 已结束的进程ID
     * @param exitCode: 进程退出码（通过/proc文件系统获取，可能不准确）
     * @note: 当监控到进程消失时发出此信号
     *
     * @example:
     * @code
     *   connect(&watcher, &ProcessWatcher::processExited, this, [](qint64 pid) {
     *       qInfo() << "Monitored process exited:" << pid;
     *   });
     * @endcode
     */
    void processExited(qint64 pid, int exitCode = -1);

    /**
     * 进程监控异常信号
     *
     * @param errorMsg: 错误消息描述
     * @note: 监控过程中出现意外错误时发出（如权限不足等）
     */
    void processError(const QString &errorMsg);

    /**
     * 定期健康检查信号 - 用于UI实时更新
     *
     * @param pid: 被监控的进程ID
     * @param alive: 进程是否存活 (true=存活，false=已结束)
     * @note: 每个检查周期发出一次此信号，可用于更新UI状态显示
     *
     * @example:
     * @code
     *   connect(&watcher, &ProcessWatcher::healthCheck, this, [](qint64 pid, bool alive) {
     *       statusLabel->setText(alive ? "运行中" : "已停止");
     *   });
     * @endcode
     */
    void healthCheck(qint64 pid, bool alive);

private slots:
    /**
     * 定时器超时槽 - 执行周期性的进程检查
     */
    void onCheckTimer();

private:
    /**
    * 检查进程是否存活
     *
     * @param pid: 进程ID
     * @return: true=进程存活，false=进程已结束
     *
    * @note: Windows 使用 OpenProcess/GetExitCodeProcess，其他平台尝试读取 /proc/[pid]/stat。
     */
    bool isProcessRunning(qint64 pid) const;

    QTimer *m_checkTimer;           ///< 定时检查计时器
    qint64 m_watchedPid;            ///< 被监控的进程ID
    bool m_isWatching;              ///< 是否正在监控中
    bool m_lastKnownAlive;          ///< 上次记录的进程存活状态
};

#endif // PROCESS_WATCHER_H
