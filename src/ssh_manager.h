#ifndef SSH_MANAGER_H
#define SSH_MANAGER_H

#include <QObject>
#include <QTimer>
#include <QString>
#include <atomic>
#include <functional>
#include <thread>
#include <libssh/libssh.h>

class SshManager : public QObject {
    Q_OBJECT
public:
    enum class State { Disconnected, Connecting, Connected };

    explicit SshManager(QObject *parent = nullptr);
    ~SshManager() override;

    void connectToHost(const QString &host, const QString &user,
                       int port, const QString &password);
    void disconnectFromHost();

    bool  isConnected() const { return m_state == State::Connected; }
    State state()       const { return m_state; }

    // 同步执行远程命令，返回标准输出；可选获取标准错误和退出码。
    bool executeCommand(const QString &cmd, QString &output,
                        QString *errorOutput = nullptr,
                        int *exitCode = nullptr,
                        int timeoutMs = 12000);

    bool uploadFile(const QString &localPath,
                    const QString &remotePath,
                    const std::function<bool(qint64, qint64)> &progressCallback,
                    QString *errorMessage = nullptr);

    // 提供 libssh session 供 ScriptRunner 打开独立 channel
    ssh_session session() const { return m_session; }

signals:
    void connected();
    void disconnected(const QString &reason);
    void connectError(const QString &message);

private slots:
    void onKeepAliveTimeout();

private:
    void doCleanup();
    void joinConnectThread();

    State       m_state   { State::Disconnected };
    ssh_session m_session { nullptr };

    std::atomic<bool> m_cancelConnect { false };
    std::atomic<unsigned int> m_connectGeneration { 0 };
    std::thread m_connectThread;

    QTimer *m_keepAliveTimer { nullptr };   // alive check (5 s)
};

#endif // SSH_MANAGER_H
