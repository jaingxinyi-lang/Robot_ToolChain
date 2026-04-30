#include "serial_manager.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QRegularExpression>
#include <QTimer>

namespace {

constexpr int kReadSliceMs = 50;
constexpr int kAsyncTailReserve = 256;

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

} // namespace

SerialManager::SerialManager(QObject *parent)
    : QObject(parent)
    , m_port(new QSerialPort(this))
    , m_asyncTimeoutTimer(new QTimer(this))
{
    m_asyncTimeoutTimer->setSingleShot(true);
    connect(m_asyncTimeoutTimer, &QTimer::timeout,
            this, &SerialManager::onAsyncTimeout);
    connect(m_port, &QSerialPort::readyRead,
            this, &SerialManager::onReadyRead);
    connect(m_port, &QSerialPort::errorOccurred,
            this, &SerialManager::onErrorOccurred);
}

SerialManager::~SerialManager()
{
    disconnectFromPort();
}

void SerialManager::connectToPort(const SerialConfig &config)
{
    if (m_state != State::Disconnected) {
        return;
    }

    m_state = State::Connecting;
    m_config = config;

    if (!configurePort(config)) {
        m_state = State::Disconnected;
        emit connectError(QString("串口参数配置失败：%1").arg(m_port->errorString()));
        return;
    }

    if (!m_port->open(QIODevice::ReadWrite)) {
        m_state = State::Disconnected;
        emit connectError(QString("串口打开失败：%1").arg(m_port->errorString()));
        return;
    }

    m_port->clear(QSerialPort::AllDirections);
    m_state = State::Connected;
    emit connected();
}

void SerialManager::disconnectFromPort()
{
    if (m_state == State::Disconnected) {
        return;
    }

    const bool wasConnected = (m_state == State::Connected);
    m_asyncTimeoutTimer->stop();
    m_asyncRunning = false;
    m_asyncSawBegin = false;
    m_asyncBuffer.clear();

    if (m_port->isOpen()) {
        m_port->close();
    }

    m_state = State::Disconnected;
    if (wasConnected) {
        emit disconnected("用户主动断开");
    }
}

bool SerialManager::executeCommand(const QString &cmd, QString &output,
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

    if (!isConnected() || !m_port->isOpen()) {
        if (errorOutput) {
            *errorOutput = "串口未连接";
        }
        return false;
    }

    if (m_asyncRunning) {
        if (errorOutput) {
            *errorOutput = "串口已有命令正在执行";
        }
        return false;
    }

    const QString token = nextToken();
    const QByteArray payload = buildWrappedCommand(cmd, token).toUtf8();

    m_port->clear(QSerialPort::Input);
    const qint64 written = m_port->write(payload);
    if (written < 0 || !m_port->waitForBytesWritten(timeoutMs > 0 ? qMin(timeoutMs, 3000) : 3000)) {
        if (errorOutput) {
            *errorOutput = QString("串口写入失败：%1").arg(m_port->errorString());
        }
        return false;
    }

    QByteArray buffer;
    QElapsedTimer elapsed;
    elapsed.start();
    const int effectiveTimeoutMs = timeoutMs > 0 ? timeoutMs : m_config.commandTimeoutMs;
    bool markerSeen = false;
    const QRegularExpression exitRegex(
        QString("__RTC_EXIT_%1_(\\d+)__").arg(QRegularExpression::escape(token)));

    while (elapsed.elapsed() < effectiveTimeoutMs) {
        if (m_port->waitForReadyRead(kReadSliceMs)) {
            buffer.append(m_port->readAll());
            const QString text = QString::fromUtf8(buffer);
            if (exitRegex.match(text).hasMatch()) {
                markerSeen = true;
                break;
            }
        }
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    const QString text = QString::fromUtf8(buffer);
    int parsedExitCode = -1;
    output = sanitizeCommandOutput(text, token, &parsedExitCode);
    if (exitCode) {
        *exitCode = parsedExitCode;
    }

    if (!markerSeen) {
        if (errorOutput) {
            *errorOutput = "串口命令执行超时";
        }
        return false;
    }

    if (parsedExitCode != 0 && errorOutput) {
        *errorOutput = output;
    }

    return true;
}

bool SerialManager::startCommand(const QString &cmd, int timeoutMs)
{
    if (!isConnected() || !m_port->isOpen()) {
        emit commandFailed("串口未连接，无法执行远程脚本");
        return false;
    }

    if (m_asyncRunning) {
        emit commandFailed("串口已有命令正在执行");
        return false;
    }

    m_asyncToken = nextToken();
    m_asyncBuffer.clear();
    m_asyncSawBegin = false;
    m_asyncRunning = true;
    m_port->clear(QSerialPort::Input);

    const QByteArray payload = buildWrappedCommand(cmd, m_asyncToken).toUtf8();
    if (m_port->write(payload) < 0) {
        m_asyncRunning = false;
        emit commandFailed(QString("串口写入失败：%1").arg(m_port->errorString()));
        return false;
    }

    const int effectiveTimeoutMs = timeoutMs > 0 ? timeoutMs : m_config.commandTimeoutMs;
    if (effectiveTimeoutMs > 0) {
        m_asyncTimeoutTimer->start(effectiveTimeoutMs);
    }
    return true;
}

bool SerialManager::stopCommand(bool force)
{
    Q_UNUSED(force)
    if (!m_asyncRunning || !m_port->isOpen()) {
        return false;
    }

    m_port->write(QByteArray(1, char(0x03)));
    m_port->waitForBytesWritten(500);
    finishAsyncCommand(130, QProcess::CrashExit);
    return true;
}

void SerialManager::onReadyRead()
{
    if (!m_asyncRunning) {
        return;
    }

    m_asyncBuffer += QString::fromUtf8(m_port->readAll());

    const QRegularExpression exitRegex(
        QString("__RTC_EXIT_%1_(\\d+)__").arg(QRegularExpression::escape(m_asyncToken)));
    const QRegularExpressionMatch match = exitRegex.match(m_asyncBuffer);
    if (match.hasMatch()) {
        const QString beforeExit = m_asyncBuffer.left(match.capturedStart());
        m_asyncBuffer = beforeExit;
        emitBufferedAsyncOutput(true);

        const int exitCode = match.captured(1).toInt();
        finishAsyncCommand(exitCode, QProcess::NormalExit);
        return;
    }

    emitBufferedAsyncOutput(false);
}

void SerialManager::onErrorOccurred(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError) {
        return;
    }

    if (m_state == State::Connecting) {
        m_state = State::Disconnected;
        emit connectError(QString("串口错误：%1").arg(m_port->errorString()));
        return;
    }

    if (m_state == State::Connected && error == QSerialPort::ResourceError) {
        m_asyncTimeoutTimer->stop();
        m_asyncRunning = false;
        if (m_port->isOpen()) {
            m_port->close();
        }
        m_state = State::Disconnected;
        emit disconnected(QString("串口连接已断开：%1").arg(m_port->errorString()));
    }
}

void SerialManager::onAsyncTimeout()
{
    if (!m_asyncRunning) {
        return;
    }

    emit errorReceived("\n串口命令执行超时\n");
    stopCommand(true);
}

QString SerialManager::buildWrappedCommand(const QString &cmd, const QString &token) const
{
    return QString("printf '\\n__RTC_BEGIN_%1__\\n'\n%2\n__rtc_exit_code=$?\nprintf '\\n__RTC_EXIT_%1_%s__\\n' \"$__rtc_exit_code\"\n")
        .arg(token, cmd);
}

QString SerialManager::sanitizeCommandOutput(const QString &text, const QString &token, int *exitCode) const
{
    if (exitCode) {
        *exitCode = -1;
    }

    QString sanitized = text;
    const QString beginMarker = QString("__RTC_BEGIN_%1__").arg(token);
    const int beginIndex = sanitized.indexOf(beginMarker);
    if (beginIndex >= 0) {
        sanitized = sanitized.mid(beginIndex + beginMarker.size());
    }

    const QRegularExpression exitRegex(
        QString("__RTC_EXIT_%1_(\\d+)__").arg(QRegularExpression::escape(token)));
    const QRegularExpressionMatch match = exitRegex.match(sanitized);
    if (match.hasMatch()) {
        if (exitCode) {
            *exitCode = match.captured(1).toInt();
        }
        sanitized = sanitized.left(match.capturedStart());
    }

    return sanitized.trimmed();
}

QString SerialManager::nextToken() const
{
    return QString::number(QDateTime::currentMSecsSinceEpoch(), 36);
}

bool SerialManager::configurePort(const SerialConfig &config)
{
    m_port->setPortName(config.portName);
    return m_port->setBaudRate(config.baudRate)
        && m_port->setDataBits(toDataBits(config.dataBits))
        && m_port->setParity(toParity(config.parity))
        && m_port->setStopBits(toStopBits(config.stopBits))
        && m_port->setFlowControl(toFlowControl(config.flowControl));
}

void SerialManager::finishAsyncCommand(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_asyncTimeoutTimer->stop();
    m_asyncRunning = false;
    m_asyncSawBegin = false;
    m_asyncBuffer.clear();
    emit commandFinished(exitCode, exitStatus);
}

void SerialManager::emitBufferedAsyncOutput(bool flushAll)
{
    const QString beginMarker = QString("__RTC_BEGIN_%1__").arg(m_asyncToken);
    if (!m_asyncSawBegin) {
        const int beginIndex = m_asyncBuffer.indexOf(beginMarker);
        if (beginIndex < 0) {
            if (m_asyncBuffer.size() > kAsyncTailReserve) {
                m_asyncBuffer = m_asyncBuffer.right(kAsyncTailReserve);
            }
            return;
        }
        m_asyncBuffer = m_asyncBuffer.mid(beginIndex + beginMarker.size());
        m_asyncSawBegin = true;
    }

    if (m_asyncBuffer.isEmpty()) {
        return;
    }

    if (flushAll || m_asyncBuffer.size() > kAsyncTailReserve) {
        const int emitLength = flushAll ? m_asyncBuffer.size()
                                        : m_asyncBuffer.size() - kAsyncTailReserve;
        const QString text = m_asyncBuffer.left(emitLength);
        m_asyncBuffer = m_asyncBuffer.mid(emitLength);
        if (!text.isEmpty()) {
            emit outputReceived(text);
        }
    }
}