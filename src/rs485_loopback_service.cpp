#include "rs485_loopback_service.h"

#include "rs485_frame_codec.h"

#include <QTimer>

namespace {

constexpr int kDefaultReconnectMs = 3000;
constexpr int kControlDrainTimeoutMs = 150;
constexpr int kControlInitGraceMs = 3000;
constexpr int kMaxBufferedBytes = Rs485FrameCodec::MaxFrameLength * 8;

QSerialPort::DataBits toDataBits(int value)
{
    switch (value) {
    case 5: return QSerialPort::Data5;
    case 6: return QSerialPort::Data6;
    case 7: return QSerialPort::Data7;
    default: return QSerialPort::Data8;
    }
}

QSerialPort::Parity toParity(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == "odd") return QSerialPort::OddParity;
    if (normalized == "even") return QSerialPort::EvenParity;
    if (normalized == "mark") return QSerialPort::MarkParity;
    if (normalized == "space") return QSerialPort::SpaceParity;
    return QSerialPort::NoParity;
}

QSerialPort::StopBits toStopBits(const QString &value)
{
    const QString normalized = value.trimmed();
    if (normalized == "1.5") return QSerialPort::OneAndHalfStop;
    if (normalized == "2") return QSerialPort::TwoStop;
    return QSerialPort::OneStop;
}

QSerialPort::FlowControl toFlowControl(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == "hardware") return QSerialPort::HardwareControl;
    if (normalized == "software") return QSerialPort::SoftwareControl;
    return QSerialPort::NoFlowControl;
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

Rs485LoopbackService::Rs485LoopbackService(QObject *parent)
    : QObject(parent)
    , m_port(new QSerialPort(this))
    , m_reconnectTimer(new QTimer(this))
    , m_idleSummaryTimer(new QTimer(this))
{
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout,
            this, &Rs485LoopbackService::onReconnectTimeout);

    m_idleSummaryTimer->setSingleShot(false);
    connect(m_idleSummaryTimer, &QTimer::timeout,
            this, &Rs485LoopbackService::onIdleSummaryTimeout);

    connect(m_port, &QSerialPort::readyRead,
            this, &Rs485LoopbackService::onReadyRead);
    connect(m_port, &QSerialPort::errorOccurred,
            this, &Rs485LoopbackService::onSerialError);
}

Rs485LoopbackService::~Rs485LoopbackService()
{
    stop(QStringLiteral("服务析构"));
}

void Rs485LoopbackService::start(const Rs485Config &config)
{
    m_config = config;
    m_reconnectTimer->stop();
    m_reconnectPending = false;
    m_silentReconnect = false;
    m_expectedInitBaudRate = 0;
    m_expectedInitDeadlineMs = 0;

    if (!m_config.enabled) {
        stop(QStringLiteral("485 服务未启用"));
        return;
    }

    if (m_config.portName.trimmed().isEmpty()) {
        stop(QStringLiteral("485 串口未配置"));
        emit errorOccurred(QStringLiteral("485 服务启动失败：串口未配置"));
        return;
    }

    if (m_running && m_port->isOpen()) {
        stop(QStringLiteral("重新启动"));
    }

    if (!openPort()) {
        scheduleReconnect();
    }
}

void Rs485LoopbackService::stop(const QString &reason)
{
    m_reconnectTimer->stop();
    m_idleSummaryTimer->stop();
    m_reconnectPending = false;
    m_expectedInitBaudRate = 0;
    m_expectedInitDeadlineMs = 0;
    m_rxBuffer.clear();

    const bool wasRunning = m_running || m_port->isOpen();
    if (m_port->isOpen()) {
        m_port->close();
    }
    m_running = false;
    m_hadTrafficThisBaud = false;

    if (wasRunning) {
        emit stopped(reason.isEmpty() ? QStringLiteral("已停止") : reason);
    }
}

void Rs485LoopbackService::restart(const Rs485Config &config)
{
    stop(QStringLiteral("应用新配置"));
    start(config);
}

bool Rs485LoopbackService::openPort()
{
    if (m_port->isOpen()) {
        m_port->close();
    }

    if (!configurePort(m_config.startBaudRate)) {
        if (!m_silentReconnect) {
            emit errorOccurred(QStringLiteral("485 串口参数配置失败：%1").arg(m_port->errorString()));
            m_silentReconnect = true;
        }
        return false;
    }

    if (!m_port->open(QIODevice::ReadWrite)) {
        if (!m_silentReconnect) {
            emit errorOccurred(QStringLiteral("485 串口打开失败：%1 (%2)")
                .arg(portDescription(), m_port->errorString()));
            m_silentReconnect = true;
        }
        return false;
    }

    m_port->clear(QSerialPort::AllDirections);
    m_rxBuffer.clear();
    m_running = true;
    m_reconnectPending = false;
    m_silentReconnect = false;
    m_expectedInitBaudRate = 0;
    m_expectedInitDeadlineMs = 0;
    resetStatisticsForBaud(m_config.startBaudRate);
    emit started(m_config.portName, m_config.startBaudRate);
    emit info(QStringLiteral("485 回环服务已启动：%1 @ %2，模式：%3")
        .arg(m_config.portName)
        .arg(m_config.startBaudRate)
        .arg(m_config.autoModeEnabled ? QStringLiteral("AUTO") : QStringLiteral("MANUAL")));
    return true;
}

bool Rs485LoopbackService::configurePort(int baudRate)
{
    m_port->setPortName(m_config.portName);
    const bool configured = m_port->setBaudRate(baudRate)
        && m_port->setDataBits(toDataBits(m_config.dataBits))
        && m_port->setParity(toParity(m_config.parity))
        && m_port->setStopBits(toStopBits(m_config.stopBits))
        && m_port->setFlowControl(toFlowControl(m_config.flowControl));
    if (configured) {
        m_statistics.currentBaudRate = baudRate;
    }
    return configured;
}

bool Rs485LoopbackService::switchBaudRate(int baudRate)
{
    if (baudRate == m_statistics.currentBaudRate) {
        return true;
    }

    m_port->clear(QSerialPort::AllDirections);
    m_rxBuffer.clear();

    if (!m_port->setBaudRate(baudRate)) {
        return false;
    }
    m_statistics.currentBaudRate = baudRate;

    m_port->waitForBytesWritten(10);
    m_port->clear(QSerialPort::AllDirections);
    m_rxBuffer.clear();

    return true;
}

void Rs485LoopbackService::scheduleReconnect()
{
    if (!m_config.enabled || m_config.portName.trimmed().isEmpty()) {
        return;
    }
    if (m_reconnectPending) {
        return;
    }

    m_running = false;
    m_reconnectPending = true;
    const int intervalMs = qMax(1000, m_config.reconnectIntervalMs > 0
                                      ? m_config.reconnectIntervalMs
                                      : kDefaultReconnectMs);
    m_reconnectTimer->start(intervalMs);
    if (!m_silentReconnect) {
        emit info(QStringLiteral("485 服务将在 %1 ms 后重试打开串口").arg(intervalMs));
    }
}

void Rs485LoopbackService::onReconnectTimeout()
{
    m_reconnectPending = false;
    if (!openPort()) {
        scheduleReconnect();
    }
}

void Rs485LoopbackService::onReadyRead()
{
    if (!m_running || !m_port->isOpen()) {
        m_port->readAll();
        return;
    }

    m_rxBuffer.append(m_port->readAll());
    trimOversizedBuffer();
    processBuffer();
}

void Rs485LoopbackService::onSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError) {
        return;
    }

    if (error == QSerialPort::ResourceError || error == QSerialPort::DeviceNotFoundError) {
        if (!m_silentReconnect) {
            emit errorOccurred(QStringLiteral("485 串口异常：%1").arg(m_port->errorString()));
            m_silentReconnect = true;
        }
        if (m_port->isOpen()) {
            m_port->close();
        }
        m_running = false;
        scheduleReconnect();
    }
}

void Rs485LoopbackService::processBuffer()
{
    while (true) {
        QByteArray frame;
        int consumedBytes = 0;
        const int frameLength = Rs485FrameCodec::tryExtractFrame(m_rxBuffer, &frame, &consumedBytes);

        if (frameLength > 0) {
            // 识别到完整协议帧，正常处理
            m_rxBuffer.remove(0, consumedBytes);
            handleFrame(frame);
            continue;
        }

        if (frameLength < 0) {
            updateFail(&Rs485Statistics::formatFail);
        }

        if (consumedBytes > 0) {
            // 无法识别为协议帧：透明回传原始数据
            const QByteArray rawEcho = m_rxBuffer.left(consumedBytes);
            m_rxBuffer.remove(0, qMin(consumedBytes, m_rxBuffer.size()));
            if (m_running && m_port->isOpen()) {
                m_port->write(rawEcho);
                m_port->flush();
                m_hadTrafficThisBaud = true;
                m_lastFrameMs = nowMs();
                if (m_config.idleSummaryMs > 0 && !m_idleSummaryTimer->isActive()) {
                    m_idleSummaryTimer->start(qMax(1000, m_config.idleSummaryMs));
                }
                emit info(QStringLiteral("485 透明回传 %1 字节：%2")
                    .arg(rawEcho.size())
                    .arg(QString::fromLocal8Bit(rawEcho).simplified()));
            }
            continue; // 继续尝试解析剩余缓冲区
        }
        break;
    }
}

void Rs485LoopbackService::handleFrame(const QByteArray &frame)
{
    m_lastFrameMs = nowMs();
    m_hadTrafficThisBaud = true;
    if (m_config.idleSummaryMs > 0 && !m_idleSummaryTimer->isActive()) {
        m_idleSummaryTimer->start(qMax(1000, m_config.idleSummaryMs));
    }

    const quint32 sequence = Rs485FrameCodec::sequenceOf(frame);
    const bool checksumOk = Rs485FrameCodec::checksumOk(frame);

    if (sequence == Rs485FrameCodec::InitSequence) {
        if (!checksumOk) {
            updateFail(&Rs485Statistics::checksumFail);
            return;
        }
        if (writeFrame(frame, kControlDrainTimeoutMs)) {
            ++m_statistics.initFrames;
            if (m_expectedInitBaudRate == m_statistics.currentBaudRate) {
                m_expectedInitBaudRate = 0;
                m_expectedInitDeadlineMs = 0;
                emit info(QStringLiteral("485 收到 RK 初始化帧，已回发确认，波特率同步完成：%1")
                    .arg(m_statistics.currentBaudRate));
            } else {
                emit info(QStringLiteral("485 收到 RK 初始化帧，已回发确认"));
            }
            emit statisticsChanged();
        } else {
            updateFail(&Rs485Statistics::sendFail);
        }
        return;
    }

    if (sequence == Rs485FrameCodec::ControlSequence
        && Rs485FrameCodec::payloadLengthOf(frame) == 4) {
        handleControlFrame(frame, checksumOk);
        return;
    }

    if (!writeFrame(frame)) {
        updateFail(&Rs485Statistics::sendFail);
        return;
    }

    if (checksumOk) {
        updateSuccess();
    } else {
        updateFail(&Rs485Statistics::checksumFail);
    }
}

bool Rs485LoopbackService::writeFrame(const QByteArray &frame, int drainTimeoutMs)
{
    if (!m_running || !m_port->isOpen()) {
        return false;
    }

    const qint64 written = m_port->write(frame);
    if (written != frame.size()) {
        return false;
    }
    m_port->flush();
    if (drainTimeoutMs > 0) {
        m_port->waitForBytesWritten(drainTimeoutMs);
    }
    return true;
}

void Rs485LoopbackService::handleControlFrame(const QByteArray &frame, bool checksumOk)
{
    if (!writeFrame(frame, kControlDrainTimeoutMs)) {
        updateFail(&Rs485Statistics::sendFail);
        return;
    }

    if (!checksumOk) {
        updateFail(&Rs485Statistics::checksumFail);
        return;
    }

    ++m_statistics.controlFrames;
    emit statisticsChanged();

    if (!m_config.autoModeEnabled) {
        return;
    }

    const int nextBaudRate = Rs485FrameCodec::controlBaudRateOf(frame);
    if (!isBaudAllowed(nextBaudRate)) {
        emit info(QStringLiteral("485 忽略未在白名单中的 AUTO 波特率：%1").arg(nextBaudRate));
        return;
    }

    if (nextBaudRate == m_statistics.currentBaudRate) {
        return;
    }

    while (m_port->bytesToWrite() > 0) {
        if (!m_port->waitForBytesWritten(kControlDrainTimeoutMs)) {
            break;
        }
    }

    emitBaudSummary(QStringLiteral("切换前"));
    if (!switchBaudRate(nextBaudRate)) {
        emit errorOccurred(QStringLiteral("485 AUTO 切换波特率失败：%1 (%2)")
            .arg(nextBaudRate)
            .arg(m_port->errorString()));
        return;
    }

    resetStatisticsForBaud(nextBaudRate);
    m_expectedInitBaudRate = nextBaudRate;
    m_expectedInitDeadlineMs = nowMs() + kControlInitGraceMs;
    emit baudChanged(nextBaudRate);
    emit info(QStringLiteral("485 AUTO 已切换波特率：%1，等待 RK INIT 帧")
        .arg(nextBaudRate));
}

void Rs485LoopbackService::updateSuccess()
{
    ++m_statistics.success;
    emit statisticsChanged();

    const int progressInterval = qMax(0, m_config.logProgressInterval);
    if (progressInterval > 0 && (m_statistics.success % static_cast<quint64>(progressInterval)) == 0) {
        emit info(QStringLiteral("485 回环进度：success=%1 fail=%2 baud=%3")
            .arg(m_statistics.success)
            .arg(m_statistics.fail)
            .arg(m_statistics.currentBaudRate));
    }
}

void Rs485LoopbackService::updateFail(quint64 Rs485Statistics::*field)
{
    ++m_statistics.fail;
    ++(m_statistics.*field);
    emit statisticsChanged();
}

void Rs485LoopbackService::resetStatisticsForBaud(int baudRate)
{
    m_statistics.success = 0;
    m_statistics.fail = 0;
    m_statistics.sendFail = 0;
    m_statistics.formatFail = 0;
    m_statistics.checksumFail = 0;
    m_statistics.currentBaudRate = baudRate;
    m_hadTrafficThisBaud = false;
    m_lastFrameMs = nowMs();
    emit statisticsChanged();
}

void Rs485LoopbackService::emitBaudSummary(const QString &prefix)
{
    const quint64 totalFrames = m_statistics.success + m_statistics.fail;
    if (totalFrames == 0) {
        return;
    }

    emit info(QStringLiteral("485 %1统计 @%2：total=%3 success=%4 fail=%5 send=%6 format=%7 checksum=%8")
        .arg(prefix)
        .arg(m_statistics.currentBaudRate)
        .arg(totalFrames)
        .arg(m_statistics.success)
        .arg(m_statistics.fail)
        .arg(m_statistics.sendFail)
        .arg(m_statistics.formatFail)
        .arg(m_statistics.checksumFail));
}

bool Rs485LoopbackService::isBaudAllowed(int baudRate) const
{
    if (baudRate <= 0) {
        return false;
    }
    return m_config.allowedBaudRates.isEmpty()
        || m_config.allowedBaudRates.contains(baudRate);
}

void Rs485LoopbackService::trimOversizedBuffer()
{
    if (m_rxBuffer.size() <= kMaxBufferedBytes) {
        return;
    }

    m_rxBuffer = m_rxBuffer.right(Rs485FrameCodec::MaxFrameLength - 1);
    updateFail(&Rs485Statistics::formatFail);
    emit errorOccurred(QStringLiteral("485 接收缓冲区超过限制，已丢弃旧数据"));
}

void Rs485LoopbackService::onIdleSummaryTimeout()
{
    const qint64 currentMs = nowMs();
    if (m_expectedInitBaudRate > 0 && currentMs < m_expectedInitDeadlineMs) {
        return;
    }

    if (!m_hadTrafficThisBaud || m_lastFrameMs <= 0) {
        return;
    }

    const int idleSummaryMs = qMax(1000, m_config.idleSummaryMs);
    if (currentMs - m_lastFrameMs < idleSummaryMs) {
        return;
    }

    emitBaudSummary(QStringLiteral("空闲"));
    m_hadTrafficThisBaud = false;

    // AUTO 模式下，空闲结束后重置回起始波特率，准备下一轮测试
    if (m_config.autoModeEnabled && m_statistics.currentBaudRate != m_config.startBaudRate) {
        if (switchBaudRate(m_config.startBaudRate)) {
            m_expectedInitBaudRate = 0;
            m_expectedInitDeadlineMs = 0;
            resetStatisticsForBaud(m_config.startBaudRate);
            emit baudChanged(m_config.startBaudRate);
            emit info(QStringLiteral("485 AUTO 空闲重置波特率 \u2192 %1\uff08等待下一轮测试\uff09")
                .arg(m_config.startBaudRate));
        }
    }
}

QString Rs485LoopbackService::portDescription() const
{
    return QStringLiteral("%1 @ %2")
        .arg(m_config.portName)
        .arg(m_statistics.currentBaudRate > 0 ? m_statistics.currentBaudRate : m_config.startBaudRate);
}