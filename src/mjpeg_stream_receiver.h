#ifndef MJPEG_STREAM_RECEIVER_H
#define MJPEG_STREAM_RECEIVER_H

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QNetworkReply>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;

/**
 * @brief 极简 MJPEG-over-HTTP 接收器。
 *
 * 通过 QNetworkAccessManager 以流式方式拉取 multipart/x-mixed-replace，
 * 按 boundary 与 Content-Length 解析 JPEG 帧并通过 frameReady 信号抛出。
 *
 * 配套板卡端 camera_mjpeg_server.py：
 *   - boundary=frame
 *   - 每帧带 Content-Type: image/jpeg 与 Content-Length
 */
class MjpegStreamReceiver : public QObject {
    Q_OBJECT

public:
    explicit MjpegStreamReceiver(QObject *parent = nullptr);
    ~MjpegStreamReceiver() override;

    bool isRunning() const { return m_reply != nullptr; }

public slots:
    void start(const QUrl &url);
    void stop();

signals:
    void streamStarted();
    void frameReady(const QImage &image);
    void streamError(const QString &message);
    void streamStopped();

private slots:
    void onReadyRead();
    void onFinished();
    void onErrorOccurred(int code);

private:
    void resetParser();
    void detectBoundaryFromHeaders();
    void processBuffer();
    bool parseHeaders(int headerEnd, qint64 &outContentLength);

    QNetworkAccessManager *m_nam{nullptr};
    QPointer<QNetworkReply> m_reply;

    QByteArray m_buffer;
    QByteArray m_boundaryMarker;   // e.g. "--frame"
    bool m_headersAnnounced{false};

    // Parser state.
    enum class State { SeekBoundary, ReadHeaders, ReadBody };
    State m_state{State::SeekBoundary};
    qint64 m_pendingContentLength{-1};

    static constexpr int kMaxBufferBytes = 8 * 1024 * 1024; // 8 MB safety cap
};

#endif // MJPEG_STREAM_RECEIVER_H
