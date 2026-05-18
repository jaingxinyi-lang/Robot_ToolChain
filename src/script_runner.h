#ifndef SCRIPT_RUNNER_H
#define SCRIPT_RUNNER_H

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <memory>
#include <libssh/libssh.h>

class CommunicationManager;
class QTextDecoder;

/**
 * @class ScriptRunner
 * @brief 脚本执行器 - 负责启动、停止脚本并捕获其输出
 *
 * 功能特性:
 * - 异步执行脚本，不阻塞UI主线程
 * - 实时捕获标准输出和错误输出
 * - 通过信号反馈脚本生命周期事件
 * - 支持脚本中止操作（SIGTERM/SIGKILL）
 *
 * 使用示例:
 * @code
 *   ScriptRunner runner;
 *   connect(&runner, &ScriptRunner::outputReceived,
 *           [](const QString& text) { qDebug() << text; });
 *   runner.startScript("/opt/robot/calibrate.sh");
 * @endcode
 */
class ScriptRunner : public QObject {
    Q_OBJECT

public:
    /**
     * 构造函数
     * @param parent: 父對象
     */
    explicit ScriptRunner(QObject *parent = nullptr);

    /**
     * 析构函数
     */
    ~ScriptRunner();

    /**
     * 启动脚本执行
     *
     * @param scriptPath: 脚本完整路径，如 "/opt/robot/calibrate.sh"
     * @param args: 脚本参数列表，默认空列表
     * @return: 成功启动返回true，失败返回false
     *
     * @note: 脚本异步执行，通过信号 started() 反馈启动结果
     * @warning: 如果上一个脚本仍在运行中，返回false
     *
     * @example:
     * @code
     *   QStringList args;
     *   args << "--motor-id=1" << "--calibrate-mode=fast";
     *   runner.startScript("/opt/robot/calibrate.sh", args);
     * @endcode
     */
    bool startScript(const QString &scriptPath, const QStringList &args = QStringList());

    /**
     * 停止脚本执行
     *
     * @param force: 是否强制杀死（SIGKILL），默认先发送SIGTERM
     * @return: 成功暂停停止返回true，失败返回false
     *
     * @note: force=false时先发送SIGTERM信号，脚本有2秒的优雅退出机会
     * @note: force=true时直接发送SIGKILL信号，强制杀死进程
     * @warning: SIGKILL无法被捕获，脚本无法执行清理操作
     */
    bool stopScript(bool force = false);

    /**
     * 获取脚本当前运行状态
     *
     * @return: true=脚本正在运行，false=脚本已停止或未启动
     */
    bool isRunning() const;

    /**
     * 获取脚本进程ID（PID）
     *
     * @return: 有效的进程ID (>0)，或-1表示未运行或无效
     */
    qint64 getProcessId() const;

    /**
     * 设置输出文本编码
     *
     * @param codec: 编码名称，如"UTF-8"、"GBK"等，默认"UTF-8"
     * @note: 建议在startScript()之前调用此函数
     */
    void setOutputEncoding(const QString &codec = "UTF-8");

    // 远程模式：通过 libssh session 在目标机执行脚本
    void setRemoteManager(CommunicationManager *mgr);
    void clearRemoteMode();
    bool isRemoteMode() const { return m_remoteMode; }

signals:
    /**
     * 脚本标准输出接收信号
     *
     * @param text: 脚本输出的文本行（可能包含换行符）
     * @note: 信号延迟 < 100ms
     *
     * @example:
     * @code
     *   connect(&runner, &ScriptRunner::outputReceived, this, [](const QString& text) {
     *       qInfo() << "Output:" << text;
     *   });
     * @endcode
     */
    void outputReceived(const QString &text);

    /**
     * 脚本错误输出接收信号
     *
     * @param text: 脚本错误输出的文本行
     * @note: 与outputReceived()独立，便于区分普通输出和错误输出
     */
    void errorReceived(const QString &text);

    /**
     * 脚本启动完成信号
     *
     * @param pid: 脚本进程ID
     * @note: 在脚本成功启动且开始执行时发出此信号
     */
    void started(qint64 pid);

    /**
     * 脚本执行完成信号
     *
     * @param exitCode: 脚本退出码（0=正常，其他值=异常）
     * @param exitStatus: 退出状态（QProcess::NormalExit=正常退出，其他=异常退出）
     * @note: 脚本正常结束或被SIGTERM杀死时发出此信号
     */
    void finished(int exitCode, QProcess::ExitStatus exitStatus);

    /**
     * 脚本异常崩溃信号
     *
     * @param errorMsg: 错误消息描述
     * @note: 当QProcess出现错误时发出（如权限不足、脚本不存在等）
     */
    void crashed(const QString &errorMsg);

private slots:
    /**
     * 处理进程标准输出就绪事件
     */
    void onReadyReadStandardOutput();

    /**
     * 处理进程错误输出就绪事件
     */
    void onReadyReadStandardError();

    /**
     * 处理进程执行完成事件
     *
     * @param exitCode: 进程退出码
     */
    void onProcessFinished(int exitCode);

    /**
     * 处理QProcess错误事件
     *
     * @param error: QProcess错误类型
     */
    void onProcessError(QProcess::ProcessError error);
    void onChannelPollTimeout();

private:
    void resetOutputDecoders();
    QString decodeOutput(const QByteArray &data, std::unique_ptr<QTextDecoder> &decoder);
    // 通过新 SSH channel 强制杀死远程进程树（处理 exec sudo 产生的孤儿进程）
    void forceKillRemoteProcessTree();

    std::unique_ptr<QProcess> m_process;  ///< 本地模式进程对象
    QString m_outputCodec;                ///< 输出文本编码方式
    std::unique_ptr<QTextDecoder> m_stdoutDecoder;
    std::unique_ptr<QTextDecoder> m_stderrDecoder;
    bool m_isRunning;                     ///< 脚本运行状态标志
    bool        m_remoteMode  { false };  ///< 是否通过 SSH 远程执行
    CommunicationManager *m_commManager { nullptr };///< 通信管理器（不拥有所有权）
    ssh_channel m_channel     { nullptr };///< 当前远程执行 channel
    QTimer     *m_pollTimer   { nullptr };///< 轮询 channel 输出（50ms）
    qint64      m_remoteShellPid { -1 }; ///< SSH 启动时捕获的远程 shell PID，用于进程树 kill
    QString     m_remoteScriptPath;      ///< 当前运行的脚本路径
};

#endif // SCRIPT_RUNNER_H
