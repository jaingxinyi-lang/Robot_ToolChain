#include "video_view.h"

#include <QPaintEvent>
#include <QPainter>

VideoView::VideoView(QWidget *parent)
    : QWidget(parent),
      m_placeholder(tr("等待视频信号...")) {
    setMinimumSize(640, 360);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    pal.setColor(QPalette::WindowText, QColor(220, 220, 220));
    setPalette(pal);
}

void VideoView::setPlaceholderText(const QString &text) {
    m_placeholder = text;
    if (m_frame.isNull()) {
        update();
    }
}

void VideoView::setFrame(const QImage &image) {
    if (image.isNull()) {
        return;
    }
    m_frame = image;
    update();
}

void VideoView::clear() {
    m_frame = QImage();
    update();
}

void VideoView::paintEvent(QPaintEvent * /*event*/) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    if (m_frame.isNull()) {
        QFont f = p.font();
        f.setPointSize(14);
        p.setFont(f);
        p.setPen(QColor(200, 200, 200));
        p.drawText(rect(), Qt::AlignCenter, m_placeholder);
        return;
    }

    const QSize target = size();
    const QSize scaled = m_frame.size().scaled(target, Qt::KeepAspectRatio);
    const int x = (target.width()  - scaled.width())  / 2;
    const int y = (target.height() - scaled.height()) / 2;
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(QRect(QPoint(x, y), scaled), m_frame);
}
