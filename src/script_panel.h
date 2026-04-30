#ifndef SCRIPT_PANEL_H
#define SCRIPT_PANEL_H

#include <QWidget>
#include <QVector>
#include "config_dialog.h"  // ScriptConfig, SCRIPT_SLOT_* 常量

class QPushButton;
class QVBoxLayout;

/**
 * @class ScriptPanel
 * @brief 独立脚本执行面板（非模态独立窗口）
 *
 * 顶部：一键配置（绿色，3份宽）+ 停止（红色，1份宽）
 * 中部：动态数量的独立脚本执行按钮
 *
 * 菜单栏"设置→脚本面板..."触发后显示此窗口。
 */
class ScriptPanel : public QWidget {
    Q_OBJECT

public:
    explicit ScriptPanel(int slotCount = SCRIPT_SLOT_DEFAULT, QWidget *parent = nullptr);
    ~ScriptPanel() override = default;

    void rebuildButtons(int slotCount);

    /**
     * @brief 用脚本配置中的名称更新各按钮文本
     */
    void updateButtonTexts(const QVector<ScriptConfig> &configs);

    /**
     * @brief 设置繁忙状态：true=脚本运行中（一键配置及单脚本按钮禁用，停止按钮启用）
     */
    void setBusy(bool busy);

signals:
    void scriptButtonClicked(int slotIndex);

private:
    void clearButtons();

    QWidget             *m_buttonContainer{nullptr};
    QVBoxLayout         *m_buttonLayout{nullptr};
    QVector<QPushButton *> m_scriptBtns;
};

#endif // SCRIPT_PANEL_H
