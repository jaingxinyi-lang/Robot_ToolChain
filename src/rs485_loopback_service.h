#ifndef RS485_LOOPBACK_SERVICE_H
#define RS485_LOOPBACK_SERVICE_H

#include "config_dialog.h"

#include <QObject>
#include <QByteArray>
#include <QDateTime>
#include <QSerialPort>

class QTimer;

struct Rs485Statistics {
    quint64 success{0};
    quint64 fail{0};
    quint64 sendFail{0};
    quint64 formatFail{0};
    quint64 checksumFail{0};
    quint64 initFrames{0};
    quint64 controlFrames{0};
    int currentBaudRate{0};
};

class Rs485LoopbackService : public QObject {
    Q_OBJECT

public:
    explicit Rs485LoopbackService(QObject *parent = nullptr);
    ~Rs485LoopbackService() override;

    void start(const Rs485Config &config);
    void stop(const QString &reason = QString());
    void restart(const Rs485Config &config);

    bool isRunning() const { return m_running; }
    Rs485Statistics statistics() const { return m_statistics; }
    int currentBaudRate() const { return m_statistics.currentBaudRate; }

signals:
    void started(const QString &portName, int baudRate);
    void stopped(const QString &reason);
    void info(const QString &message);
    void errorOccurred(const QString &message);
    void baudChanged(int baudRate);
    void statisticsChanged();

private slots:
    void onReadyRead();
    void onSerialError(QSerialPort::SerialPortError error);
    void onReconnectTimeout();
    void onIdleSummaryTimeout();

private:
    bool openPort();
    bool configurePort(int baudRate);
    bool switchBaudRate(int baudRate);
    void scheduleReconnect();
    void processBuffer();
    void handleFrame(const QByteArray &frame);
    bool writeFrame(const QByteArray &frame, int drainTimeoutMs = 0);
    void handleControlFrame(const QByteArray &frame, bool checksumOk);
    void updateSuccess();
    void updateFail(quint64 Rs485Statistics::*field);
    void resetStatisticsForBaud(int baudRate);
    void emitBaudSummary(const QString &prefix);
    bool isBaudAllowed(int baudRate) const;
    void trimOversizedBuffer();
    QString portDescription() const;

    QSerialPort *m_port{nullptr};
    QTimer *m_reconnectTimer{nullptr};
    QTimer *m_idleSummaryTimer{nullptr};
    Rs485Config m_config;
    QByteArray m_rxBuffer;
    Rs485Statistics m_statistics;
    bool m_running{false};
    bool m_reconnectPending{false};
    bool m_hadTrafficThisBaud{false};
    bool m_silentReconnect{false};  // 已报过一次错，后续重试静默
    int m_expectedInitBaudRate{0};
    qint64 m_expectedInitDeadlineMs{0};
    qint64 m_lastFrameMs{0};
};

#endif // RS485_LOOPBACK_SERVICE_H