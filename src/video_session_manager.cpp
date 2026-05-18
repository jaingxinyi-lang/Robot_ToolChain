/**
 * @file video_session_manager.cpp
 * @brief 摄像头视频会话管理器实现
 *
 * 状态机：
 *
 *   [空闲] ──beginSession()──▶ [等待握手]
 *                                   │
 *                         feedLine() 逐行解析：
 *                           · BOARD_VIDEO_PORT=<n>  → m_port
 *                           · BOARD_VIDEO_PATH=<p>  → m_path
 *                           · BOARD_IP=<ip>         → m_boardIp
 *                           · VIDEO_READY           → tryStartStream()
 *                                   │
 *                              tryStartStream()
 *                           合法性检查通过后启动 MjpegStreamReceiver
 *                                   │
 *                              [流运行中]
 *                                   │
 *                           stopSession() 或脚本结束
 *                                   │
 *                              [空闲]
 */

#include "video_session_manager.h"
#include "mjpeg_stream_receiver.h"
#include "video_view.h"

#include <QUrl>

VideoSessionManager::VideoSessionManager(MjpegStreamReceiver *receiver,
                                         VideoView           *view,
                                         QObject             *parent)
    : QObject(parent)
    , m_receiver(receiver)
    , m_view(view)
{
    if (m_receiver) {
        connect(m_receiver, &MjpegStreamReceiver::streamStarted,
                this, &VideoSessionManager::onReceiverStreamStarted);
        connect(m_receiver, &MjpegStreamReceiver::streamError,
                this, &VideoSessionManager::onReceiverStreamError);
    }
}

void VideoSessionManager::beginSession(const QString &sshHostOverride)
{
    m_sessionActive    = true;
    m_streamReady      = false;
    m_sshHostOverride  = sshHostOverride.trimmed();
    m_boardIp.clear();
    m_port = 0;
    m_path.clear();

    if (m_view) {
        m_view->clear();
        m_view->setPlaceholderText(QStringLiteral("正在启动摄像头推流…"));
    }

    emit sessionStarted();
}

void VideoSessionManager::feedLine(const QString &line)
{
    // 仅在会话激活且流未就绪时解析
    if (!m_sessionActive || m_streamReady) {
        return;
    }
    parseHandshakeLine(line);
}

void VideoSessionManager::stopSession()
{
    if (!m_sessionActive && !m_streamReady) {
        return;
    }

    if (m_receiver) {
        m_receiver->stop();
    }
    if (m_view) {
        m_view->clear();
        m_view->setPlaceholderText(QStringLiteral("等待视频信号..."));
        m_view->hide();
    }

    m_sessionActive   = false;
    m_streamReady     = false;
    m_sshHostOverride.clear();
    m_boardIp.clear();
    m_port = 0;
    m_path.clear();

    emit sessionEnded();
}

// ── 私有方法 ─────────────────────────────────────────────────────────────────

void VideoSessionManager::parseHandshakeLine(const QString &line)
{
    const QString t = line.trimmed();
    if (t.isEmpty()) return;

    if (t.startsWith(QStringLiteral("BOARD_VIDEO_PORT="))) {
        const QString val = t.mid(QStringLiteral("BOARD_VIDEO_PORT=").size()).trimmed();
        bool ok = false;
        const int p = val.toInt(&ok);
        if (ok && p > 0 && p <= 65535) {
            m_port = p;
        }
        return;
    }
    if (t.startsWith(QStringLiteral("BOARD_VIDEO_PATH="))) {
        m_path = t.mid(QStringLiteral("BOARD_VIDEO_PATH=").size()).trimmed();
        return;
    }
    if (t.startsWith(QStringLiteral("BOARD_IP="))) {
        m_boardIp = t.mid(QStringLiteral("BOARD_IP=").size()).trimmed();
        return;
    }
    if (t == QStringLiteral("VIDEO_READY")) {
        tryStartStream();
        return;
    }
}

void VideoSessionManager::tryStartStream()
{
    if (m_streamReady) return;

    if (m_port <= 0 || m_path.isEmpty()) {
        emit streamError(QStringLiteral("收到 VIDEO_READY 但握手字段不全，放弃视频显示"));
        return;
    }

    // SSH 模式优先用通信配置里的主机名；串口模式用 BOARD_IP。
    QString host = m_sshHostOverride;
    if (host.isEmpty()) {
        host = m_boardIp.trimmed();
    }
    if (host.isEmpty() || host == QLatin1String("0.0.0.0")) {
        emit streamError(QStringLiteral("无法确定板卡 IP，无法显示视频"));
        return;
    }

    QString path = m_path;
    if (!path.startsWith('/')) path.prepend('/');

    const QString full = QStringLiteral("http://%1:%2%3").arg(host).arg(m_port).arg(path);
    const QUrl url(full);
    if (!url.isValid()) {
        emit streamError(QStringLiteral("视频地址无效: %1").arg(full));
        return;
    }

    m_streamReady = true;

    if (m_view) {
        m_view->setPlaceholderText(QStringLiteral("正在接收视频…"));
        m_view->show();
    }

    // 通知 MainWindow 执行 QSplitter 宽度分配（VideoSessionManager 不持有 Splitter 引用）
    emit streamConnecting();

    if (m_receiver) {
        m_receiver->start(url);
    }
}

void VideoSessionManager::onReceiverStreamStarted()
{
    emit streamStarted();
}

void VideoSessionManager::onReceiverStreamError(const QString &message)
{
    emit streamError(message);
}
