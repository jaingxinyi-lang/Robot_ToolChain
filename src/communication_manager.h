#ifndef COMMUNICATION_MANAGER_H
#define COMMUNICATION_MANAGER_H

#include "config_dialog.h"
#include "ssh_manager.h"

#include <QObject>

class CommunicationManager : public QObject {
    Q_OBJECT

public:
    enum class State { Disconnected, Connecting, Connected };

    explicit CommunicationManager(QObject *parent = nullptr);
    ~CommunicationManager() override = default;

    void setConfig(const CommunicationConfig &config);
    CommunicationConfig config() const { return m_config; }

    CommunicationMode mode() const { return CommunicationMode::Ssh; }
    QString modeName() const;
    QString connectedActionText() const;
    QString disconnectedActionText() const;

    void connectCurrent();
    void disconnectCurrent();

    bool isConnected() const;
    State state() const;

    bool executeCommand(const QString &cmd, QString &output,
                        QString *errorOutput = nullptr,
                        int *exitCode = nullptr,
                        int timeoutMs = 12000);

    SshManager *sshManager() const { return m_sshManager; }
    ssh_session sshSession() const;

signals:
    void connected();
    void disconnected(const QString &reason);
    void connectError(const QString &message);

private:
    SshManager *m_sshManager{nullptr};
    CommunicationConfig m_config;
};

#endif // COMMUNICATION_MANAGER_H