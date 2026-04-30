#include "mjpeg_stream_receiver.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QVariant>

namespace {
constexpr const char *kCrlf = "\r\n";
constexpr const char *kCrlfCrlf = "\r\n\r\n";
}

MjpegStreamReceiver::MjpegStreamReceiver(QObject *parent)
    : QObject(parent),
      m_nam(new QNetworkAccessManager(this)) {}

MjpegStreamReceiver::~MjpegStreamReceiver() {
    stop();
}

void MjpegStreamReceiver::start(const QUrl &url) {
    if (m_reply) {
        stop();
    }
    if (!url.isValid()) {
        emit streamError(tr("无效的视频地址: %1").arg(url.toString()));
        return;
    }

    resetParser();

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "Robot_ToolChain/1.0");
    req.setRawHeader("Accept", "multipart/x-mixed-replace, image/jpeg");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    m_reply = m_nam->get(req);
    if (!m_reply) {
        emit streamError(tr("无法发起视频请求"));
        return;
    }

    connect(m_reply, &QNetworkReply::readyRead,
            this, &MjpegStreamReceiver::onReadyRead);
    connect(m_reply, &QNetworkReply::finished,
            this, &MjpegStreamReceiver::onFinished);
    connect(m_reply,
            QOverload<QNetworkReply::NetworkError>::of(&QNetworkReply::errorOccurred),
            this, [this](QNetworkReply::NetworkError code) {
                onErrorOccurred(static_cast<int>(code));
            });
}

void MjpegStreamReceiver::stop() {
    if (!m_reply) {
        return;
    }
    QNetworkReply *reply = m_reply.data();
    m_reply.clear();
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
    resetParser();
    emit streamStopped();
}

void MjpegStreamReceiver::resetParser() {
    m_buffer.clear();
    m_boundaryMarker.clear();
    m_headersAnnounced = false;
    m_state = State::SeekBoundary;
    m_pendingContentLength = -1;
}

void MjpegStreamReceiver::detectBoundaryFromHeaders() {
    if (!m_reply) return;
    const QByteArray ct = m_reply->rawHeader("Content-Type");
    // e.g. multipart/x-mixed-replace; boundary=frame
    int idx = ct.indexOf("boundary=");
    QByteArray boundary;
    if (idx >= 0) {
        boundary = ct.mid(idx + int(sizeof("boundary=") - 1));
        // Strip optional surrounding quotes / trailing params.
        if (!boundary.isEmpty() && boundary.startsWith('"')) {
            boundary = boundary.mid(1);
            int end = boundary.indexOf('"');
            if (end >= 0) boundary = boundary.left(end);
        } else {
            int sc = boundary.indexOf(';');
            if (sc >= 0) boundary = boundary.left(sc);
        }
        boundary = boundary.trimmed();
    }
    if (boundary.isEmpty()) {
        boundary = "frame"; // server default
    }
    m_boundaryMarker = "--" + boundary;
}

void MjpegStreamReceiver::onReadyRead() {
    if (!m_reply) return;

    if (!m_headersAnnounced) {
        detectBoundaryFromHeaders();
        m_headersAnnounced = true;
        emit streamStarted();
    }

    const QByteArray chunk = m_reply->readAll();
    if (chunk.isEmpty()) return;

    if (m_buffer.size() + chunk.size() > kMaxBufferBytes) {
        emit streamError(tr("MJPEG 缓冲超过上限，放弃流"));
        stop();
        return;
    }
    m_buffer.append(chunk);

    processBuffer();
}

bool MjpegStreamReceiver::parseHeaders(int headerEnd, qint64 &outContentLength) {
    // headerEnd points at the first byte of the CRLFCRLF terminator.
    const QByteArray headers = m_buffer.left(headerEnd);
    outContentLength = -1;

    int pos = 0;
    while (pos < headers.size()) {
        int eol = headers.indexOf(kCrlf, pos);
        if (eol < 0) eol = headers.size();
        const QByteArray line = headers.mid(pos, eol - pos);
        pos = eol + 2;

        const int colon = line.indexOf(':');
        if (colon <= 0) continue;
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (name == "content-length") {
            bool ok = false;
            const qint64 v = value.toLongLong(&ok);
            if (ok && v >= 0) outContentLength = v;
        }
    }
    return true;
}

void MjpegStreamReceiver::processBuffer() {
    while (true) {
        if (m_state == State::SeekBoundary) {
            if (m_boundaryMarker.isEmpty()) return;
            const int idx = m_buffer.indexOf(m_boundaryMarker);
            if (idx < 0) {
                // Discard everything but a trailing window (size of boundary)
                // to allow matching across chunk boundaries.
                if (m_buffer.size() > m_boundaryMarker.size()) {
                    m_buffer.remove(0, m_buffer.size() - m_boundaryMarker.size());
                }
                return;
            }
            // Drop bytes up to and including the boundary marker.
            int after = idx + m_boundaryMarker.size();
            // Skip optional trailing CRLF directly after boundary.
            if (m_buffer.size() >= after + 2 &&
                m_buffer.mid(after, 2) == kCrlf) {
                after += 2;
            }
            m_buffer.remove(0, after);
            m_state = State::ReadHeaders;
            m_pendingContentLength = -1;
            continue;
        }

        if (m_state == State::ReadHeaders) {
            const int sep = m_buffer.indexOf(kCrlfCrlf);
            if (sep < 0) return; // wait for more
            qint64 contentLen = -1;
            parseHeaders(sep, contentLen);
            m_buffer.remove(0, sep + 4); // strip headers + CRLFCRLF
            if (contentLen <= 0) {
                // Server didn't supply Content-Length. Without it we'd have
                // to scan for next boundary, which is expensive and brittle.
                emit streamError(tr("MJPEG 帧缺少 Content-Length"));
                stop();
                return;
            }
            m_pendingContentLength = contentLen;
            m_state = State::ReadBody;
            continue;
        }

        if (m_state == State::ReadBody) {
            if (m_pendingContentLength < 0) {
                m_state = State::SeekBoundary;
                continue;
            }
            if (qint64(m_buffer.size()) < m_pendingContentLength) {
                return; // wait for more
            }
            const QByteArray jpeg = m_buffer.left(int(m_pendingContentLength));
            m_buffer.remove(0, int(m_pendingContentLength));
            m_pendingContentLength = -1;

            QImage img;
            if (img.loadFromData(jpeg, "JPG") && !img.isNull()) {
                emit frameReady(img);
            }
            // After the body there may be CRLF then the next boundary; strip CRLF if present.
            if (m_buffer.startsWith(kCrlf)) {
                m_buffer.remove(0, 2);
            }
            m_state = State::SeekBoundary;
            continue;
        }
    }
}

void MjpegStreamReceiver::onFinished() {
    if (m_reply) {
        QNetworkReply *reply = m_reply.data();
        m_reply.clear();
        reply->deleteLater();
    }
    resetParser();
    emit streamStopped();
}

void MjpegStreamReceiver::onErrorOccurred(int code) {
    if (!m_reply) return;
    const QString msg = m_reply->errorString();
    emit streamError(tr("视频流错误(%1): %2").arg(code).arg(msg));
    // finished() will fire and clean up.
}
