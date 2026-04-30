#include "process_watcher.h"
#include <QDebug>
#include <QFile>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

ProcessWatcher::ProcessWatcher(QObject *parent)
    : QObject(parent)
    , m_checkTimer(new QTimer(this))
    , m_watchedPid(-1)
    , m_isWatching(false)
    , m_lastKnownAlive(false)
{
    // 连接定时器超时信号
    connect(m_checkTimer, &QTimer::timeout, this, &ProcessWatcher::onCheckTimer);
}

ProcessWatcher::~ProcessWatcher()
{
    stopWatching();
}

bool ProcessWatcher::watchProcess(qint64 pid, unsigned int checkIntervalMs)
{
    // 参数验证
    if (pid <= 0) {
        emit processError("Invalid PID: " + QString::number(pid));
        return false;
    }

    if (checkIntervalMs < 100) {
        qWarning() << "Check interval too short:" << checkIntervalMs
                   << "ms, setting to 100ms minimum";
        checkIntervalMs = 100;
    }

    // 如果已在监控其他进程，先停止
    if (m_isWatching) {
        stopWatching();
    }

    m_watchedPid = pid;
    m_isWatching = true;
    m_lastKnownAlive = true;

    qInfo() << "Starting to watch process:" << pid << "with interval:" << checkIntervalMs << "ms";

    // 启动定时检查
    m_checkTimer->start(checkIntervalMs);
    return true;
}

void ProcessWatcher::stopWatching()
{
    if (m_checkTimer->isActive()) {
        m_checkTimer->stop();
    }

    m_isWatching = false;
    m_watchedPid = -1;
    m_lastKnownAlive = false;

    qInfo() << "Process watcher stopped";
}

bool ProcessWatcher::isProcessAlive(qint64 pid) const
{
    return isProcessRunning(pid);
}

qint64 ProcessWatcher::getWatchedPid() const
{
    return m_watchedPid;
}

bool ProcessWatcher::isWatching() const
{
    return m_isWatching;
}

void ProcessWatcher::onCheckTimer()
{
    if (!m_isWatching || m_watchedPid <= 0) {
        qWarning() << "Invalid watcher state";
        stopWatching();
        return;
    }

    bool currentlyAlive = isProcessRunning(m_watchedPid);

    // 发出健康检查信号（每次检查都发出）
    emit healthCheck(m_watchedPid, currentlyAlive);

    // 检测状态变化（从存活变为不存活）
    if (m_lastKnownAlive && !currentlyAlive) {
        qInfo() << "Detected process exit:" << m_watchedPid;
        m_lastKnownAlive = false;
        stopWatching();

        // 发出进程已结束信号
        emit processExited(m_watchedPid, -1);
    } else if (!m_lastKnownAlive && currentlyAlive) {
        // 进程意外复活（极少见情况）
        qWarning() << "Process unexpectedly came back to life:" << m_watchedPid;
        m_lastKnownAlive = true;
    }
}

bool ProcessWatcher::isProcessRunning(qint64 pid) const
{
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        return false;
    }

    DWORD exitCode = 0;
    const bool alive = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    // Linux: 检查/proc/[pid]/stat 是否存在
    QString procPath = QString("/proc/%1/stat").arg(pid);
    QFile procFile(procPath);

    if (procFile.exists() && procFile.open(QIODevice::ReadOnly)) {
        // 文件存在且可读，说明进程存活
        procFile.close();
        return true;
    }

    // 如果/proc文件不存在或无法打开，进程已结束
    return false;
#endif
}
