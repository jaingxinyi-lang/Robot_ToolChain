#ifndef VIDEO_VIEW_H
#define VIDEO_VIEW_H

#include <QImage>
#include <QString>
#include <QWidget>

class QPaintEvent;

/**
 * @brief 视频显示控件：等比缩放居中绘制最新帧；无帧时显示提示文字。
 */
class VideoView : public QWidget {
    Q_OBJECT

public:
    explicit VideoView(QWidget *parent = nullptr);

    void setPlaceholderText(const QString &text);

public slots:
    void setFrame(const QImage &image);
    void clear();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage m_frame;
    QString m_placeholder;
};

#endif // VIDEO_VIEW_H
