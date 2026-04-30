#ifndef TEST_RESULT_CARD_H
#define TEST_RESULT_CARD_H

#include <QFrame>
#include <QString>

class QLabel;

class TestResultCard : public QFrame {
    Q_OBJECT

public:
    enum class Status { Pending, Success, Failure };

    explicit TestResultCard(const QString &name, QWidget *parent = nullptr);
    void setStatus(Status status);
    QString name() const { return m_name; }

private:
    QString statusText(Status status) const;
    QString statusColor(Status status) const;
    QString borderColor(Status status) const;

    QString m_name;
    QLabel *m_indicator{nullptr};
    QLabel *m_nameLabel{nullptr};
    QLabel *m_statusLabel{nullptr};
    Status m_status{Status::Pending};
};

#endif // TEST_RESULT_CARD_H