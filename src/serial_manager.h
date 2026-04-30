#ifndef SERIAL_MANAGER_H
#define SERIAL_MANAGER_H

#include "config_dialog.h"

#include <QObject>
#include <QProcess>
#include <QSerialPort>
#include <QString>

class QTimer;

class SerialManager : public QObject {
    Q_OBJECT

public:
    enum class State { Disconnected, Connecting, Connected };

    explicit SerialManager(QObject *parent = nullptr);
    ~SerialManager() override;

    void connectToPort(const SerialConfig &config);
    void disconnectFromPort();

    bool isConnected() const { return m_state == State::Connected; }
    State state() const { return m_state; }

    bool executeCommand(const QString &cmd, QString &output,
                        QString *errorOutput = nullptr,
                        int *exitCode = nullptr,
                        int timeoutMs = 12000);

    bool startCommand(const QString &cmd, int timeoutMs = 0);
    bool stopCommand(bool force = false);
    bool isCommandRunning() const { return m_asyncRunning; }

signals:
    void connected();
    void disconnected(const QString &reason);
    void connectError(const QString &message);
    void outputReceived(const QString &text);
    void errorReceived(const QString &text);
    void commandFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void commandFailed(const QString &message);

private slots:
    void onReadyRead();
    void onErrorOccurred(QSerialPort::SerialPortError error);
    void onAsyncTimeout();

private:
    QString buildWrappedCommand(const QString &cmd, const QString &token) const;
    QString sanitizeCommandOutput(const QString &text, const QString &token, int *exitCode) const;
    QString nextToken() const;
    bool configurePort(const SerialConfig &config);
    void finishAsyncCommand(int exitCode, QProcess::ExitStatus exitStatus);
    void emitBufferedAsyncOutput(bool flushAll);

    QSerialPort *m_port{nullptr};
    QTimer *m_asyncTimeoutTimer{nullptr};
    SerialConfig m_config;
    State m_state{State::Disconnected};

    bool m_asyncRunning{false};
    bool m_asyncSawBegin{false};
    QString m_asyncToken;
    QString m_asyncBuffer;
};

#endif // SERIAL_MANAGER_H