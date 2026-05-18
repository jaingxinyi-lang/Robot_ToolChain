#ifndef VIDEO_SESSION_MANAGER_H
#define VIDEO_SESSION_MANAGER_H

/**
 * @file video_session_manager.h
 * @brief 摄像头视频会话管理器
 *
 * 负责从脚本输出中解析握手字段，并在握手完成后启动/停止 MJPEG 接收。
 *
 * 握手协议（脚本 stdout 逐行上报，顺序任意，VIDEO_READY 必须最后出现）：
 *   BOARD_VIDEO_PORT=<port>    视频流监听端口
 *   BOARD_VIDEO_PATH=<path>    HTTP 路径（如 /?action=stream）
 *   BOARD_IP=<ip>              板卡 IP（串口模式必须；SSH 模式由调用方提供）
 *   VIDEO_READY                握手完成，触发流启动
 *
 * 使用方法：
 *   1. 连接 VideoSessionManager 的信号到 MainWindow 的槽，处理 UI 动作。
 *   2. 脚本启动时调用 beginSession(sshHostOverride)。
 *   3. 每行脚本输出喂给 feedLine(line)。
 *   4. 脚本结束时调用 stopSession()。
 */

#include <QObject>
#include <QUrl>

class MjpegStreamReceiver;
class VideoView;

class VideoSessionManager : public QObject
{
    Q_OBJECT

public:
    explicit VideoSessionManager(MjpegStreamReceiver *receiver,
                                 VideoView           *view,
                                 QObject             *parent = nullptr);

    /**
     * @brief 激活视频会话，等待握手字段
     * @param sshHostOverride SSH 模式下使用此 IP/主机名；留空则等待 BOARD_IP 字段
     */
    void beginSession(const QString &sshHostOverride = {});

    /**
     * @brief 喂入脚本输出的单行文本，解析握手字段
     * @note  仅在会话激活且流未就绪时有效
     */
    void feedLine(const QString &line);

    /** @brief 停止会话：停止接收器、重置所有状态 */
    void stopSession();

    bool isSessionActive() const { return m_sessionActive; }
    bool isStreamReady()   const { return m_streamReady;   }

signals:
    /** 视频会话已激活（脚本已识别为摄像头脚本） */
    void sessionStarted();
    /**
     * 握手完成，即将建立 MJPEG 连接（此时 VideoView 已 show 出占位符）
     * MainWindow 应在此信号中完成 QSplitter 宽度分配。
     */
    void streamConnecting();
    /** MJPEG 流连接成功 */
    void streamStarted();
    /** MJPEG 流发生错误 */
    void streamError(const QString &message);
    /** 会话已结束（stopSession 调用后发出） */
    void sessionEnded();

private slots:
    void onReceiverStreamStarted();
    void onReceiverStreamError(const QString &message);

private:
    void parseHandshakeLine(const QString &line);
    void tryStartStream();

    MjpegStreamReceiver *m_receiver{nullptr};  // 不拥有，由 MainWindow 管理生命周期
    VideoView           *m_view{nullptr};       // 不拥有

    bool    m_sessionActive{false};
    bool    m_streamReady{false};
    QString m_sshHostOverride;   // SSH 模式：来自通信配置的主机名/IP
    QString m_boardIp;           // 串口模式：脚本上报的 BOARD_IP
    int     m_port{0};
    QString m_path;
};

#endif // VIDEO_SESSION_MANAGER_H
