/**
 * @file script_runner.cpp
 * @brief 脚本执行引擎 —— 统一本地与远程两种执行模式
 *
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │  执行模式                                                            │
 * ├─────────────────────────────────────────────────────────────────────┤
 * │  本地模式（m_remoteMode = false）                                    │
 * │    启动: QProcess 调用 bash/setsid 运行脚本                          │
 * │    输出: readyReadStandardOutput/Error 信号 → onReadyReadStdXxx     │
 * │    结束: QProcess::finished 信号 → onProcessFinished                │
 * │    停止: process.terminate() / process.kill()                       │
 * │                                                                     │
 * │  远程 SSH 模式（CommunicationMode::Ssh）                             │
 * │    启动: ssh_channel_open_session                                   │
 * │       1. 申请 PTY（xterm-256color）                                  │
 * │          目的：使 Ctrl+C (0x03) 经由终端行规发送 SIGINT 给整个        │
 * │          前台进程组，行为等同 MobaXterm 手动按 Ctrl+C                │
 * │       2. 命令前注入 "printf '__RSCR_PID:%d\n' $$"                   │
 * │          目的：捕获 shell 的 PID，供 forceKillRemoteProcessTree 定位  │
 * │          整个进程树（PGID = $$）                                      │
 * │       3. ssh_channel_request_exec(finalCmd)                        │
 * │    轮询: m_pollTimer(50ms) → onChannelPollTimeout                   │
 * │       · 过滤并移除 __RSCR_PID:<n> 行，保存到 m_remoteShellPid       │
 * │       · PTY 将 \n 转成 \r\n，统一规范化为 \n 后再 emit outputReceived │
 * │       · 检测 ssh_channel_is_eof → 读取退出码 → emit finished        │
 * │    停止: 向 PTY 写 "\x03" (Ctrl+C) → SIGINT 发给前台进程组          │
 * │       force=true 时额外调 forceKillRemoteProcessTree()：             │
 * │         kill -9 -- -PGID   杀整个进程组                             │
 * │         pkill -9 -P PID    杀直接子进程                             │
 * │         pkill -9 -f NAME   按脚本名兜底                             │
 * │                                                                     │
 * └─────────────────────────────────────────────────────────────────────┘
 */
#include "script_runner.h"
#include "communication_manager.h"
#include <QDebug>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextCodec>
#include <QThread>

namespace {

QString shellQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace("'", "'\"'\"'");
    return "'" + escaped + "'";
}

struct ScriptLaunchContext {
    QString workingDirectory;
    QString scriptCommandPath;
};

ScriptLaunchContext buildScriptLaunchContext(const QString &scriptPath)
{
    const int slashIndex = qMax(scriptPath.lastIndexOf('/'), scriptPath.lastIndexOf('\\'));
    if (slashIndex < 0) {
        return {".", scriptPath};
    }

    const QString workingDirectory = slashIndex == 0 ? scriptPath.left(1)
                                                     : scriptPath.left(slashIndex);
    const QString fileName = scriptPath.mid(slashIndex + 1);
    const QString scriptCommandPath = fileName.isEmpty() ? scriptPath
                                                         : "./" + fileName;

    return {workingDirectory.isEmpty() ? "." : workingDirectory, scriptCommandPath};
}

} // namespace

ScriptRunner::ScriptRunner(QObject *parent)
    : QObject(parent)
    , m_process(std::make_unique<QProcess>())
    , m_outputCodec("UTF-8")
    , m_isRunning(false)
    , m_pollTimer(new QTimer(this))
{
    m_pollTimer->setInterval(50);
    m_pollTimer->setSingleShot(false);
    connect(m_pollTimer, &QTimer::timeout,
            this, &ScriptRunner::onChannelPollTimeout);

    // 连接进程信号到槽函数
    connect(m_process.get(), &QProcess::readyReadStandardOutput,
            this, &ScriptRunner::onReadyReadStandardOutput);
    connect(m_process.get(), &QProcess::readyReadStandardError,
            this, &ScriptRunner::onReadyReadStandardError);
    connect(m_process.get(), static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, &ScriptRunner::onProcessFinished);
    connect(m_process.get(), static_cast<void(QProcess::*)(QProcess::ProcessError)>(&QProcess::errorOccurred),
            this, &ScriptRunner::onProcessError);
}

ScriptRunner::~ScriptRunner()
{
    // 析构时如果进程仍在运行，停止它
    if (m_isRunning) {
        if (m_remoteMode && m_channel) {
            ssh_channel_request_send_signal(m_channel, "KILL");
            ssh_channel_close(m_channel);
            ssh_channel_free(m_channel);
            m_channel = nullptr;
        } else if (m_process && m_process->state() == QProcess::Running) {
            m_process->terminate();
            if (!m_process->waitForFinished(2000)) {
                m_process->kill();
            }
        }
    }
}
bool ScriptRunner::startScript(const QString &scriptPath, const QStringList &args)
{
    // 防护：如果脚本已在运行中，拒绝重复启动
    if (m_isRunning) {
        qWarning() << "Script is already running. PID:" << m_process->processId();
        return false;
    }

    qInfo() << "Starting script:" << scriptPath << "with args:" << args;
    resetOutputDecoders();

    if (m_remoteMode) {
        if (!m_commManager || !m_commManager->isConnected()) {
            emit crashed("通信未连接，无法执行远程脚本");
            return false;
        }

        const ScriptLaunchContext launchContext = buildScriptLaunchContext(scriptPath);
        QString cmd = "cd " + shellQuote(launchContext.workingDirectory)
                    + " && bash " + shellQuote(launchContext.scriptCommandPath);
        for (const QString &arg : args) {
            cmd += " ";
            cmd += shellQuote(arg);
        }

        // 远程模式：通过 libssh channel 在目标机执行
        if (!m_commManager->sshSession()) {
            emit crashed("SSH 未连接，无法执行远程脚本");
            return false;
        }

        m_channel = ssh_channel_new(m_commManager->sshSession());
        if (!m_channel || ssh_channel_open_session(m_channel) != SSH_OK) {
            if (m_channel) { ssh_channel_free(m_channel); m_channel = nullptr; }
            emit crashed("打开 SSH channel 失败");
            return false;
        }

        // 申请 PTY：使信号通过终端行规发送到整个前台进程组（等同 MobaXterm Ctrl+C 行为）
        // 申请失败时降级运行，仍可用 pkill 兜底
        if (ssh_channel_request_pty_size(m_channel, "xterm-256color", 200, 50) != SSH_OK) {
            qWarning() << "SSH PTY 申请失败，进程树 kill 可能受限";
        }

        // 在命令前输出 shell PID，供强制停止时定位整个进程树
        m_remoteScriptPath = scriptPath;
        m_remoteShellPid   = -1;
        const QString finalCmd = "printf '__RSCR_PID:%d\\n' $$; exec sh -c " + shellQuote(cmd);
        if (ssh_channel_request_exec(m_channel, finalCmd.toUtf8().constData()) != SSH_OK) {
            ssh_channel_close(m_channel);
            ssh_channel_free(m_channel);
            m_channel = nullptr;
            emit crashed("SSH channel 执行命令失败");
            return false;
        }

        m_isRunning = true;
        emit started(0);
        m_pollTimer->start();
        return true;
    } else {
        // 本地模式
#ifdef Q_OS_WIN
        // Windows：直接用 bash（Git Bash / WSL）执行，无 setsid
        m_process->setProgram("bash");
        QStringList fullArgs = {scriptPath};
        fullArgs += args;
        m_process->setArguments(fullArgs);
#else
        // Linux：用 setsid 将脚本放入新的会话和进程组
        m_process->setProgram("setsid");
        QStringList fullArgs = {"/bin/bash", scriptPath};
        fullArgs += args;
        m_process->setArguments(fullArgs);
#endif

        // 设置进程继承父进程的环境变量
        m_process->setProcessEnvironment(QProcessEnvironment::systemEnvironment());

        // 启动进程
        m_process->start();

        // 检查进程是否成功启动
        if (!m_process->waitForStarted()) {
            m_isRunning = false;
            qCritical() << "Failed to start process:" << scriptPath;
            emit crashed("脚本启动失败，请检查脚本路径和执行权限");
            return false;
        }

        m_isRunning = true;
        qint64 pid = m_process->processId();
        qInfo() << "Process started successfully. PID:" << pid;
        emit started(pid);
        return true;
    }
}

bool ScriptRunner::stopScript(bool force)
{
    if (!m_isRunning) {
        qWarning() << "No process running";
        return false;
    }

    if (m_remoteMode && m_channel) {
        // 通过 PTY 行规发 Ctrl+C（SIGINT），杀死整个前台进程组（bash/sudo/子进程全部收到）
        ssh_channel_write(m_channel, "\x03", 1);
        if (force) {
            // 额外通过新 channel pkill 兜底，处理 ignore SIGINT 的进程
            forceKillRemoteProcessTree();
            ssh_channel_request_send_signal(m_channel, "KILL");
        } else {
            ssh_channel_request_send_signal(m_channel, "TERM");
        }
        // channel 由 onChannelPollTimeout 检测 EOF 后自动关闭
        return true;
    }

    if (!m_process) return false;

    if (force) {
        // 强制杀死（SIGKILL）
        qInfo() << "Force killing process:" << m_process->processId();
        m_process->kill();
    } else {
        // 优雅终止（SIGTERM）
        qInfo() << "Terminating process:" << m_process->processId();
        m_process->terminate();

        // 等待2秒让进程优雅退出
        if (!m_process->waitForFinished(2000)) {
            qWarning() << "Process did not terminate gracefully, force killing";
            m_process->kill();
        }
    }

    return true;
}

bool ScriptRunner::isRunning() const
{
    if (m_remoteMode) {
        return m_isRunning && m_channel != nullptr;
    }

    return m_isRunning && m_process && (m_process->state() == QProcess::Running);
}

qint64 ScriptRunner::getProcessId() const
{
    if (m_process) {
        return m_process->processId();
    }
    return -1;
}

void ScriptRunner::setOutputEncoding(const QString &codec)
{
    m_outputCodec = codec;
    resetOutputDecoders();
    qInfo() << "Output encoding set to:" << codec;
}

void ScriptRunner::onReadyReadStandardOutput()
{
    if (!m_process) {
        return;
    }

    // 读取进程所有可用的标准输出
    QByteArray output = m_process->readAllStandardOutput();
    if (!output.isEmpty()) {
        const QString text = decodeOutput(output, m_stdoutDecoder);

        if (!text.isEmpty()) {
            emit outputReceived(text);
        }
    }
}

void ScriptRunner::onReadyReadStandardError()
{
    if (!m_process) {
        return;
    }

    // 读取进程所有可用的错误输出
    QByteArray error = m_process->readAllStandardError();
    if (!error.isEmpty()) {
        const QString text = decodeOutput(error, m_stderrDecoder);

        if (!text.isEmpty()) {
            emit errorReceived(text);
        }
    }
}

void ScriptRunner::onProcessFinished(int exitCode)
{
    m_isRunning = false;
    QProcess::ExitStatus exitStatus = m_process->exitStatus();

    QString statusStr = (exitStatus == QProcess::NormalExit) ? "NormalExit" : "CrashExit";
    qInfo() << "Process finished. Exit code:" << exitCode << "Status:" << statusStr;

    emit finished(exitCode, exitStatus);
}

void ScriptRunner::onProcessError(QProcess::ProcessError error)
{
    m_isRunning = false;

    QString errorMsg;
    switch (error) {
    case QProcess::FailedToStart:
        errorMsg = "脚本启动失败 - 文件不存在或无执行权限";
        break;
    case QProcess::Crashed:
        errorMsg = "脚本进程崩溃";
        break;
    case QProcess::Timedout:
        errorMsg = "脚本执行超时";
        break;
    case QProcess::ReadError:
        errorMsg = "读取进程输出失败";
        break;
    case QProcess::WriteError:
        errorMsg = "写入进程输入失败";
        break;
    default:
        errorMsg = "脚本执行出现未知错误";
        break;
    }

    qCritical() << "ScriptRunner error:" << errorMsg;
    emit crashed(errorMsg);
}

void ScriptRunner::setRemoteManager(CommunicationManager *mgr)
{
    m_remoteMode  = true;
    m_commManager = mgr;
}

void ScriptRunner::clearRemoteMode()
{
    m_remoteMode  = false;
    m_commManager = nullptr;
}

void ScriptRunner::onChannelPollTimeout()
{
    if (!m_channel) return;

    char buf[4096];
    int  nbytes;

    // 读取 stdout，同时过滤启动时注入的 PID 标记行
    while ((nbytes = ssh_channel_read_nonblocking(m_channel, buf, sizeof(buf), 0)) > 0) {
        QString text = decodeOutput(QByteArray(buf, nbytes), m_stdoutDecoder);
        if (text.isEmpty()) continue;

        // PTY 将 \n 转成 \r\n，规范化为 \n
        text.replace("\r\n", "\n").replace('\r', '\n');

        // 提取 __RSCR_PID:<n> 标记（只需捕获一次），将该行从输出中移除
        if (m_remoteShellPid < 0) {
            static const QRegularExpression pidRe("__RSCR_PID:(\\d+)\\r?\\n?");
            const QRegularExpressionMatch m = pidRe.match(text);
            if (m.hasMatch()) {
                m_remoteShellPid = m.captured(1).toLongLong();
                text.remove(m.capturedStart(), m.capturedLength());
            }
        }

        if (!text.isEmpty()) {
            emit outputReceived(text);
        }
    }
    // 读取 stderr
    while ((nbytes = ssh_channel_read_nonblocking(m_channel, buf, sizeof(buf), 1)) > 0) {
        const QString text = decodeOutput(QByteArray(buf, nbytes), m_stderrDecoder);
        if (!text.isEmpty()) {
            emit errorReceived(text);
        }
    }

    // 检测 EOF（远程命令已结束）
    if (ssh_channel_is_eof(m_channel)) {
        m_pollTimer->stop();
        uint32_t exitCode = 0;
        char    *exitSignal = nullptr;
        int      coreDumped = 0;
        ssh_channel_get_exit_state(m_channel, &exitCode, &exitSignal, &coreDumped);
        ssh_channel_close(m_channel);
        ssh_channel_free(m_channel);
        m_channel    = nullptr;
        m_isRunning  = false;
        m_remoteShellPid = -1;
        emit finished(static_cast<int>(exitCode),
                      exitSignal == nullptr && coreDumped == 0 ? QProcess::NormalExit
                                                               : QProcess::CrashExit);
    }
}

void ScriptRunner::resetOutputDecoders()
{
    m_stdoutDecoder.reset();
    m_stderrDecoder.reset();
}

QString ScriptRunner::decodeOutput(const QByteArray &data, std::unique_ptr<QTextDecoder> &decoder)
{
    if (!decoder) {
        QTextCodec *codec = QTextCodec::codecForName(m_outputCodec.toUtf8());
        if (!codec) {
            codec = QTextCodec::codecForName("UTF-8");
        }
        decoder.reset(codec->makeDecoder());
    }
    return decoder->toUnicode(data);
}

void ScriptRunner::forceKillRemoteProcessTree()
{
    if (!m_commManager || !m_commManager->sshSession()) return;

    // 构建 kill 命令：
    // 1. kill -9 -- -PGID  杀死整个进程组（处理 exec sudo 后 PGID 未变的情况）
    // 2. pkill -9 -P PID   杀死直接子进程（处理 sudo fork 出的子 bash）
    // 3. pkill -9 -f NAME  按脚本名兜底（处理进程树跨 session 的情况）
    QString killCmd;
    if (m_remoteShellPid > 0) {
        killCmd = QString("kill -9 -- -%1 2>/dev/null; pkill -9 -P %1 2>/dev/null; ")
                      .arg(m_remoteShellPid);
    }
    if (!m_remoteScriptPath.isEmpty()) {
        const QString name = QFileInfo(m_remoteScriptPath).fileName();
        killCmd += QString("pkill -9 -f '%1' 2>/dev/null; ").arg(name);
    }
    if (killCmd.isEmpty()) return;
    killCmd += "true";

    ssh_channel kc = ssh_channel_new(m_commManager->sshSession());
    if (!kc) return;
    if (ssh_channel_open_session(kc) != SSH_OK) {
        ssh_channel_free(kc);
        return;
    }
    if (ssh_channel_request_exec(kc, killCmd.toUtf8().constData()) == SSH_OK) {
        // 等待 kill 命令在板端执行完毕
        QThread::msleep(300);
    }
    ssh_channel_close(kc);
    ssh_channel_free(kc);
    m_remoteShellPid = -1;
}
