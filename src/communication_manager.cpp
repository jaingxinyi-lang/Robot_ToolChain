#include "communication_manager.h"

CommunicationManager::CommunicationManager(QObject *parent)
    : QObject(parent)
    , m_sshManager(new SshManager(this))
{
    connect(m_sshManager, &SshManager::connected,
            this, &CommunicationManager::connected);
    connect(m_sshManager, &SshManager::disconnected,
            this, &CommunicationManager::disconnected);
    connect(m_sshManager, &SshManager::connectError,
            this, &CommunicationManager::connectError);
}

void CommunicationManager::setConfig(const CommunicationConfig &config)
{
    m_config = config;
}

QString CommunicationManager::modeName() const
{
    return "SSH";
}

QString CommunicationManager::connectedActionText() const
{
    return QString("%1断开").arg(modeName());
}

QString CommunicationManager::disconnectedActionText() const
{
    return QString("%1连接").arg(modeName());
}

void CommunicationManager::connectCurrent()
{
    m_sshManager->connectToHost(m_config.ssh.host,
                                m_config.ssh.user,
                                m_config.ssh.port,
                                m_config.ssh.password);
}

void CommunicationManager::disconnectCurrent()
{
    m_sshManager->disconnectFromHost();
}

bool CommunicationManager::isConnected() const
{
    return m_sshManager->isConnected();
}

CommunicationManager::State CommunicationManager::state() const
{
    switch (m_sshManager->state()) {
    case SshManager::State::Connected: return State::Connected;
    case SshManager::State::Connecting: return State::Connecting;
    case SshManager::State::Disconnected: return State::Disconnected;
    }

    return State::Disconnected;
}

bool CommunicationManager::executeCommand(const QString &cmd, QString &output,
                                          QString *errorOutput,
                                          int *exitCode,
                                          int timeoutMs)
{
    return m_sshManager->executeCommand(cmd, output, errorOutput, exitCode, timeoutMs);
}

ssh_session CommunicationManager::sshSession() const
{
    return m_sshManager->session();
}