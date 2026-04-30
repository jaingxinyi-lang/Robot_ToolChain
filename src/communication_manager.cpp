#include "communication_manager.h"

CommunicationManager::CommunicationManager(QObject *parent)
    : QObject(parent)
    , m_sshManager(new SshManager(this))
    , m_serialManager(new SerialManager(this))
{
    connect(m_sshManager, &SshManager::connected,
            this, &CommunicationManager::connected);
    connect(m_sshManager, &SshManager::disconnected,
            this, &CommunicationManager::disconnected);
    connect(m_sshManager, &SshManager::connectError,
            this, &CommunicationManager::connectError);

    connect(m_serialManager, &SerialManager::connected,
            this, &CommunicationManager::connected);
    connect(m_serialManager, &SerialManager::disconnected,
            this, &CommunicationManager::disconnected);
    connect(m_serialManager, &SerialManager::connectError,
            this, &CommunicationManager::connectError);
}

void CommunicationManager::setConfig(const CommunicationConfig &config)
{
    m_config = config;
}

QString CommunicationManager::modeName() const
{
    return m_config.mode == CommunicationMode::Serial ? "串口" : "SSH";
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
    if (m_config.mode == CommunicationMode::Serial) {
        m_serialManager->connectToPort(m_config.serial);
        return;
    }

    m_sshManager->connectToHost(m_config.ssh.host,
                                m_config.ssh.user,
                                m_config.ssh.port,
                                m_config.ssh.password);
}

void CommunicationManager::disconnectCurrent()
{
    if (m_config.mode == CommunicationMode::Serial) {
        m_serialManager->disconnectFromPort();
        return;
    }

    m_sshManager->disconnectFromHost();
}

bool CommunicationManager::isConnected() const
{
    return m_config.mode == CommunicationMode::Serial
        ? m_serialManager->isConnected()
        : m_sshManager->isConnected();
}

CommunicationManager::State CommunicationManager::state() const
{
    if (m_config.mode == CommunicationMode::Serial) {
        switch (m_serialManager->state()) {
        case SerialManager::State::Connected: return State::Connected;
        case SerialManager::State::Connecting: return State::Connecting;
        case SerialManager::State::Disconnected: return State::Disconnected;
        }
    }

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
    if (m_config.mode == CommunicationMode::Serial) {
        return m_serialManager->executeCommand(cmd, output, errorOutput, exitCode, timeoutMs);
    }

    return m_sshManager->executeCommand(cmd, output, errorOutput, exitCode, timeoutMs);
}

bool CommunicationManager::startSerialCommand(const QString &cmd, int timeoutMs)
{
    if (m_config.mode != CommunicationMode::Serial) {
        return false;
    }
    return m_serialManager->startCommand(cmd, timeoutMs);
}

bool CommunicationManager::stopSerialCommand(bool force)
{
    if (m_config.mode != CommunicationMode::Serial) {
        return false;
    }
    return m_serialManager->stopCommand(force);
}

ssh_session CommunicationManager::sshSession() const
{
    return m_sshManager->session();
}