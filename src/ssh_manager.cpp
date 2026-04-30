#include "ssh_manager.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <fcntl.h>
#include <libssh/sftp.h>
#include <sys/stat.h>
#include <sys/types.h>

// ── 构造 / 析构 ──────────────────────────────────────────────────────────────

SshManager::SshManager(QObject *parent)
    : QObject(parent)
    , m_keepAliveTimer(new QTimer(this))
{
    m_keepAliveTimer->setInterval(5000);
    m_keepAliveTimer->setSingleShot(false);

    connect(m_keepAliveTimer, &QTimer::timeout,
            this, &SshManager::onKeepAliveTimeout);
}

SshManager::~SshManager()
{
    m_cancelConnect.store(true);
    m_connectGeneration.fetch_add(1);
    joinConnectThread();
    doCleanup();
}

// ── 公开接口 ─────────────────────────────────────────────────────────────────

void SshManager::connectToHost(const QString &host, const QString &user,
                                int port, const QString &password)
{
    if (m_state != State::Disconnected) {
        return;
    }

    joinConnectThread();
    m_state = State::Connecting;
    m_cancelConnect.store(false);
    const unsigned int generation = m_connectGeneration.fetch_add(1) + 1;

    // 在后台线程执行阻塞连接，避免冻结 UI
    m_connectThread = std::thread([this, host, user, port, password, generation]() {
        const auto cancelled = [this, generation]() {
            return m_cancelConnect.load() || m_connectGeneration.load() != generation;
        };

        ssh_session sess = ssh_new();
        if (!sess) {
            QMetaObject::invokeMethod(this, [this, generation]() {
                if (m_connectGeneration.load() != generation) {
                    return;
                }
                m_state = State::Disconnected;
                emit connectError("libssh 初始化失败（内存不足）");
            }, Qt::QueuedConnection);
            return;
        }

        // 配置连接参数
        const std::string hostStd = host.toStdString();
        const std::string userStd = user.toStdString();
        const int         portVal = port;
        const long        timeout = 15L;
        const int         noStrict = 0;

        ssh_options_set(sess, SSH_OPTIONS_HOST,    hostStd.c_str());
        ssh_options_set(sess, SSH_OPTIONS_USER,    userStd.c_str());
        ssh_options_set(sess, SSH_OPTIONS_PORT,    &portVal);
        ssh_options_set(sess, SSH_OPTIONS_TIMEOUT, &timeout);
        ssh_options_set(sess, SSH_OPTIONS_STRICTHOSTKEYCHECK, &noStrict);

        if (cancelled()) { ssh_free(sess); return; }

        // 建立连接
        if (ssh_connect(sess) != SSH_OK) {
            if (cancelled()) {
                ssh_free(sess); return;
            }
            const QString errMsg = QString("SSH 连接失败：%1").arg(ssh_get_error(sess));
            ssh_free(sess);
            QMetaObject::invokeMethod(this, [this, errMsg, generation]() {
                if (m_connectGeneration.load() != generation) {
                    return;
                }
                m_state = State::Disconnected;
                emit connectError(errMsg);
            }, Qt::QueuedConnection);
            return;
        }

        if (cancelled()) { ssh_disconnect(sess); ssh_free(sess); return; }

        // 密码认证
        const std::string pwdStd = password.toStdString();
        if (ssh_userauth_password(sess, nullptr, pwdStd.c_str()) != SSH_AUTH_SUCCESS) {
            if (cancelled()) { ssh_disconnect(sess); ssh_free(sess); return; }
            const QString errMsg = QString("SSH 认证失败：%1").arg(ssh_get_error(sess));
            ssh_disconnect(sess);
            ssh_free(sess);
            QMetaObject::invokeMethod(this, [this, errMsg, generation]() {
                if (m_connectGeneration.load() != generation) {
                    return;
                }
                m_state = State::Disconnected;
                emit connectError(errMsg);
            }, Qt::QueuedConnection);
            return;
        }

        // 连接成功，回到主线程
        QMetaObject::invokeMethod(this, [this, sess, generation]() {
            if (m_cancelConnect.load() || m_connectGeneration.load() != generation) {
                ssh_disconnect(sess);
                ssh_free(sess);
                return;
            }
            m_session = sess;
            m_state   = State::Connected;
            m_keepAliveTimer->start();
            emit connected();
        }, Qt::QueuedConnection);
    });
}

void SshManager::disconnectFromHost()
{
    if (m_state == State::Disconnected) return;

    State prevState = m_state;
    m_cancelConnect.store(true);
    m_connectGeneration.fetch_add(1);
    doCleanup();
    joinConnectThread();

    if (prevState != State::Connecting) {
        emit disconnected("用户主动断开");
    }
}

bool SshManager::executeCommand(const QString &cmd, QString &output,
                                QString *errorOutput,
                                int *exitCode,
                                int timeoutMs)
{
    output.clear();
    if (errorOutput) {
        errorOutput->clear();
    }
    if (exitCode) {
        *exitCode = -1;
    }

    if (!m_session) return false;

    ssh_channel channel = ssh_channel_new(m_session);
    if (!channel) return false;

    bool success = false;
    QByteArray stdoutResult;
    QByteArray stderrResult;

    do {
        if (ssh_channel_open_session(channel) != SSH_OK) break;
        if (ssh_channel_request_exec(channel, cmd.toUtf8().constData()) != SSH_OK) break;

        char buf[4096];
        QElapsedTimer elapsed;
        elapsed.start();

        while (elapsed.elapsed() < timeoutMs) {
            bool receivedData = false;

            for (;;) {
                const int stdoutBytes = ssh_channel_read_nonblocking(channel, buf, sizeof(buf), 0);
                if (stdoutBytes > 0) {
                    stdoutResult.append(buf, stdoutBytes);
                    receivedData = true;
                    continue;
                }
                if (stdoutBytes == SSH_ERROR) {
                    break;
                }
                break;
            }

            for (;;) {
                const int stderrBytes = ssh_channel_read_nonblocking(channel, buf, sizeof(buf), 1);
                if (stderrBytes > 0) {
                    stderrResult.append(buf, stderrBytes);
                    receivedData = true;
                    continue;
                }
                if (stderrBytes == SSH_ERROR) {
                    break;
                }
                break;
            }

            if (ssh_channel_is_closed(channel) || ssh_channel_is_eof(channel)) {
                break;
            }

            if (!receivedData) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        uint32_t commandExitCode = 0;
        char    *exitSignal = nullptr;
        int      coreDumped = 0;
        if (ssh_channel_get_exit_state(channel, &commandExitCode, &exitSignal, &coreDumped) == SSH_OK) {
            if (exitCode) {
                *exitCode = static_cast<int>(commandExitCode);
            }
            success = (exitSignal == nullptr && coreDumped == 0);
        }
    } while (false);

    output = QString::fromUtf8(stdoutResult);
    if (errorOutput) {
        *errorOutput = QString::fromUtf8(stderrResult);
    }

    if (!success && errorOutput && errorOutput->isEmpty()) {
        *errorOutput = QString::fromUtf8(ssh_get_error(m_session));
    }

    ssh_channel_close(channel);
    ssh_channel_free(channel);
    return success;
}

bool SshManager::uploadFile(const QString &localPath,
                            const QString &remotePath,
                            const std::function<bool(qint64, qint64)> &progressCallback,
                            QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }

    if (!m_session || m_state != State::Connected) {
        if (errorMessage) {
            *errorMessage = "SSH 未连接";
        }
        return false;
    }

    QFile localFile(localPath);
    if (!localFile.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QString("无法打开本地文件：%1").arg(localFile.errorString());
        }
        return false;
    }

    sftp_session sftp = sftp_new(m_session);
    if (!sftp) {
        if (errorMessage) {
            *errorMessage = QString("创建 SFTP 会话失败：%1").arg(ssh_get_error(m_session));
        }
        return false;
    }

    if (sftp_init(sftp) != SSH_OK) {
        if (errorMessage) {
            *errorMessage = QString("初始化 SFTP 失败：%1").arg(sftp_get_error(sftp));
        }
        sftp_free(sftp);
        return false;
    }

    sftp_file remoteFile = sftp_open(sftp,
                                     remotePath.toUtf8().constData(),
                                     O_WRONLY | O_CREAT | O_TRUNC,
                                     S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (!remoteFile) {
        if (errorMessage) {
            *errorMessage = QString("打开远程文件失败：%1").arg(sftp_get_error(sftp));
        }
        sftp_free(sftp);
        return false;
    }

    const qint64 totalBytes = QFileInfo(localFile).size();
    qint64 sentBytes = 0;
    bool success = true;
    while (!localFile.atEnd()) {
        const QByteArray chunk = localFile.read(64 * 1024);
        if (chunk.isEmpty() && localFile.error() != QFile::NoError) {
            success = false;
            if (errorMessage) {
                *errorMessage = QString("读取本地文件失败：%1").arg(localFile.errorString());
            }
            break;
        }

        const ssize_t written = sftp_write(remoteFile, chunk.constData(), chunk.size());
        if (written != chunk.size()) {
            success = false;
            if (errorMessage) {
                *errorMessage = QString("写入远程文件失败：%1").arg(sftp_get_error(sftp));
            }
            break;
        }

        sentBytes += written;
        if (progressCallback) {
            if (!progressCallback(sentBytes, totalBytes)) {
                success = false;
                if (errorMessage) {
                    *errorMessage = "上传已取消";
                }
                break;
            }
        }
    }

    if (sftp_close(remoteFile) != SSH_OK && success) {
        success = false;
        if (errorMessage) {
            *errorMessage = QString("关闭远程文件失败：%1").arg(sftp_get_error(sftp));
        }
    }
    sftp_free(sftp);
    return success;
}

// ── 私有：清理（不发信号）────────────────────────────────────────────────────

void SshManager::doCleanup()
{
    m_keepAliveTimer->stop();

    if (m_session) {
        ssh_disconnect(m_session);
        ssh_free(m_session);
        m_session = nullptr;
    }

    m_state = State::Disconnected;
}

void SshManager::joinConnectThread()
{
    if (m_connectThread.joinable()) {
        m_connectThread.join();
    }
}

// ── 私有 Slots ────────────────────────────────────────────────────────────────

void SshManager::onKeepAliveTimeout()
{
    if (!m_session) return;

    // 比反复创建 channel 更轻的保活探测。
    if (!ssh_is_connected(m_session) || ssh_send_ignore(m_session, "keepalive") != SSH_OK) {
        doCleanup();
        emit disconnected("SSH 连接已断开（keep-alive 检测失败）");
    }
}
