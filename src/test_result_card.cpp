#include "test_result_card.h"

#include <QHBoxLayout>
#include <QLabel>

TestResultCard::TestResultCard(const QString &name, QWidget *parent)
    : QFrame(parent)
    , m_name(name)
    , m_indicator(new QLabel(this))
    , m_nameLabel(new QLabel(name, this))
    , m_statusLabel(new QLabel(this))
{
    setFrameShape(QFrame::StyledPanel);
    setMinimumSize(190, 48);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_indicator->setFixedSize(16, 16);

    m_nameLabel->setWordWrap(false);
    m_nameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_nameLabel->setStyleSheet("font-size: 11pt; font-weight: 700; color: #1f2933;");

    m_statusLabel->setMinimumWidth(58);
    m_statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_statusLabel->setStyleSheet("font-size: 10pt; font-weight: 600; color: #52606d;");

    auto *titleLayout = new QHBoxLayout();
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(8);
    titleLayout->addWidget(m_indicator, 0, Qt::AlignVCenter);
    titleLayout->addWidget(m_nameLabel, 1, Qt::AlignVCenter);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(12);
    layout->addLayout(titleLayout, 1);
    layout->addWidget(m_statusLabel, 0, Qt::AlignRight | Qt::AlignVCenter);

    setStatus(Status::Pending);
}

void TestResultCard::setStatus(Status status)
{
    m_status = status;
    const QString color = statusColor(status);
    m_indicator->setStyleSheet(QString(
        "QLabel { background: %1; border-radius: 8px; border: 1px solid rgba(0,0,0,0.14); }")
        .arg(color));
    m_statusLabel->setText(statusText(status));
    setStyleSheet(QString(
        "TestResultCard { background: #ffffff; border: 1px solid %1; border-radius: 8px; }")
        .arg(borderColor(status)));
}

QString TestResultCard::statusText(Status status) const
{
    switch (status) {
    case Status::Success: return "成功";
    case Status::Failure: return "失败";
    case Status::Pending: return "未测试";
    }
    return "未测试";
}

QString TestResultCard::statusColor(Status status) const
{
    switch (status) {
    case Status::Success: return "#2ecc71";
    case Status::Failure: return "#e74c3c";
    case Status::Pending: return "#5a9ad7";
    }
    return "#5a9ad7";
}

QString TestResultCard::borderColor(Status status) const
{
    switch (status) {
    case Status::Success: return "#a7e5bd";
    case Status::Failure: return "#f3b4ad";
    case Status::Pending: return "#bed7f0";
    }
    return "#bed7f0";
}