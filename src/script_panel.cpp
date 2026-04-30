#include "script_panel.h"

#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QFrame>
#include <QSizePolicy>

// 脚本按钮统一样式
static const char *kButtonStyle = R"(
QPushButton {
    background-color: #3A7BD5;
    color: white;
    border: none;
    border-radius: 6px;
    padding: 6px 12px;
    font-size: 20px;
    font-weight: bold;
    text-align: center;
}
QPushButton:hover {
    background-color: #4D8FE8;
}
QPushButton:pressed {
    background-color: #2860B0;
}
QPushButton:disabled {
    background-color: #A0AEC0;
    color: #E2E8F0;
}
)";

ScriptPanel::ScriptPanel(int slotCount, QWidget *parent)
    : QWidget(parent)
{
    // ── 容器背景 ────────────────────────────────────────────────
    setStyleSheet("QWidget { background-color: #F8FAFC; }");

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    // ── 标题区 ──────────────────────────────────────────────────
    auto *headerLabel = new QLabel("脚本列表", this);
    headerLabel->setStyleSheet(R"(
        QLabel {
            background-color: #2860B0;
            color: #FFFFFF;
            font-size: 24px;
            font-weight: bold;
            padding: 8px 12px;
        }
    )");
    headerLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    mainLayout->addWidget(headerLabel);

    // ── 分隔线 ──────────────────────────────────────────────────
    auto *separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Plain);
    separator->setStyleSheet("QFrame { color: #CBD5E0; }");
    mainLayout->addWidget(separator);

    // ── 按钮区 ──────────────────────────────────────────────────
    m_buttonContainer = new QWidget(this);
    m_buttonContainer->setStyleSheet("QWidget { background-color: #F8FAFC; }");
    m_buttonLayout = new QVBoxLayout(m_buttonContainer);
    m_buttonLayout->setSpacing(8);
    m_buttonLayout->setContentsMargins(12, 12, 12, 12);

    mainLayout->addWidget(m_buttonContainer, 1);

    rebuildButtons(slotCount);

    setBusy(false);
}

void ScriptPanel::clearButtons()
{
    while (QLayoutItem *item = m_buttonLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
    m_scriptBtns.clear();
}

void ScriptPanel::rebuildButtons(int slotCount)
{
    clearButtons();

    const int clampedCount = qBound(SCRIPT_SLOT_MIN, slotCount, SCRIPT_SLOT_MAX);
    for (int i = 0; i < clampedCount; ++i) {
        auto *btn = new QPushButton(QString("脚本 %1").arg(i + 1), m_buttonContainer);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        btn->setMinimumHeight(40);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(kButtonStyle);
        m_scriptBtns.push_back(btn);
        m_buttonLayout->addWidget(btn, 1);

        connect(btn, &QPushButton::clicked, this, [this, i]() {
            emit scriptButtonClicked(i);
        });
    }
}

void ScriptPanel::updateButtonTexts(const QVector<ScriptConfig> &configs)
{
    for (int i = 0; i < m_scriptBtns.size(); ++i) {
        const QString name = i < configs.size() ? configs[i].name : QString();
        m_scriptBtns[i]->setText(name.isEmpty() ? QString("脚本 %1").arg(i + 1) : name);
    }
}

void ScriptPanel::setBusy(bool busy)
{
    for (QPushButton *button : m_scriptBtns) {
        button->setEnabled(!busy);
    }
}

