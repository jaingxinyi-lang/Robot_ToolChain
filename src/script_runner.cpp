#include "script_runner.h"
#include "communication_manager.h"
#include <QDebug>
#include <QTextCodec>

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
        } else if (m_remoteMode && m_commManager && m_commManager->mode() == CommunicationMode::Serial) {
            m_commManager->stopSerialCommand(true);
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

        if (m_commManager->mode() == CommunicationMode::Serial) {
            if (!m_commManager->startSerialCommand(cmd)) {
                emit crashed("串口命令启动失败");
                return false;
            }
            m_isRunning = true;
            emit started(0);
            return true;
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

        if (ssh_channel_request_exec(m_channel, cmd.toUtf8().constData()) != SSH_OK) {
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
        // 远程模式：向远程进程发送信号
        ssh_channel_request_send_signal(m_channel, force ? "KILL" : "TERM");
        // channel 由 onChannelPollTimeout 检测 EOF 后自动关闭
        return true;
    }

    if (m_remoteMode && m_commManager && m_commManager->mode() == CommunicationMode::Serial) {
        return m_commManager->stopSerialCommand(force);
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
        if (m_commManager && m_commManager->mode() == CommunicationMode::Serial) {
            return m_isRunning && m_commManager->serialManager()->isCommandRunning();
        }
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
    disconnectSerialSignals();
    if (m_commManager && m_commManager->serialManager()) {
        m_serialConnections.push_back(connect(m_commManager->serialManager(), &SerialManager::outputReceived,
                                              this, &ScriptRunner::onSerialOutputReceived));
        m_serialConnections.push_back(connect(m_commManager->serialManager(), &SerialManager::errorReceived,
                                              this, &ScriptRunner::onSerialErrorReceived));
        m_serialConnections.push_back(connect(m_commManager->serialManager(), &SerialManager::commandFinished,
                                              this, &ScriptRunner::onSerialCommandFinished));
        m_serialConnections.push_back(connect(m_commManager->serialManager(), &SerialManager::commandFailed,
                                              this, &ScriptRunner::onSerialCommandFailed));
    }
}

void ScriptRunner::clearRemoteMode()
{
    disconnectSerialSignals();
    m_remoteMode  = false;
    m_commManager = nullptr;
}

void ScriptRunner::onChannelPollTimeout()
{
    if (!m_channel) return;

    char buf[4096];
    int  nbytes;

    // 读取 stdout
    while ((nbytes = ssh_channel_read_nonblocking(m_channel, buf, sizeof(buf), 0)) > 0) {
        const QString text = decodeOutput(QByteArray(buf, nbytes), m_stdoutDecoder);
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
        emit finished(static_cast<int>(exitCode),
                      exitSignal == nullptr && coreDumped == 0 ? QProcess::NormalExit
                                                               : QProcess::CrashExit);
    }
}

void ScriptRunner::onSerialOutputReceived(const QString &text)
{
    emit outputReceived(text);
}

void ScriptRunner::onSerialErrorReceived(const QString &text)
{
    emit errorReceived(text);
}

void ScriptRunner::onSerialCommandFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!m_isRunning) {
        return;
    }
    m_isRunning = false;
    emit finished(exitCode, exitStatus);
}

void ScriptRunner::onSerialCommandFailed(const QString &message)
{
    m_isRunning = false;
    emit crashed(message);
}

void ScriptRunner::disconnectSerialSignals()
{
    for (const QMetaObject::Connection &connection : m_serialConnections) {
        disconnect(connection);
    }
    m_serialConnections.clear();
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
