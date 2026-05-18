#ifndef CONFIG_DIALOG_H
#define CONFIG_DIALOG_H

#include <QDialog>
#include <QString>
#include <QVector>

class QLineEdit;
class QLabel;
class QPushButton;
class QTabWidget;
class QComboBox;
class QCheckBox;
class QStackedWidget;
class SshManager;
class CommunicationManager;

/**
 * @brief 脚本槽位数量边界
 */
inline constexpr int SCRIPT_SLOT_MIN = 1;
inline constexpr int SCRIPT_SLOT_MAX = 64;
inline constexpr int SCRIPT_SLOT_DEFAULT = 6;

/**
 * @struct ScriptConfig
 * @brief 脚本配置结构
 */
struct ScriptConfig {
    QString name;           ///< 脚本名称
    QString path;           ///< 脚本路径
    QString args;           ///< 附加参数（整体作为一个参数传递）
    unsigned int timeoutMs { 0 }; ///< 执行超时（毫秒）
};

/**
 * @struct SshConfig
 * @brief SSH 连接配置
 */
struct SshConfig {
    QString host     { "192.168.55.102" };
    QString user     { "noetix" };
    int     port     { 22 };
    QString password { "NoetixBumi@230915!!" };
};

enum class CommunicationMode {
    Ssh
};

struct Rs485Config {
    bool enabled { true };
    QString portName;
    int startBaudRate { 115200 };
    int dataBits { 8 };
    QString parity { "none" };
    QString stopBits { "1" };
    QString flowControl { "none" };
    bool autoModeEnabled { true };
    QVector<int> allowedBaudRates { 115200, 921600, 1000000, 1500000 };
    int reconnectIntervalMs { 3000 };
    int idleSummaryMs { 5000 };
    int logProgressInterval { 100 };
};

struct CommunicationConfig {
    SshConfig ssh;
    Rs485Config rs485;
};

/**
 * @class ConfigDialog
 * @brief 脚本配置对话框（代码驱动，支持动态脚本槽位）
 */
class ConfigDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConfigDialog(int initialSlotCount = SCRIPT_SLOT_DEFAULT, QWidget *parent = nullptr);
    ~ConfigDialog() override = default;

    void getScriptConfigs(QVector<ScriptConfig> &configs) const;
    void setScriptConfigs(const QVector<ScriptConfig> &configs);
    int currentSlotCount() const;

    unsigned int getProcessCheckInterval() const;
    void setProcessCheckInterval(unsigned int intervalMs);

    /**
    * @brief 在打开对话框前注入 SSH 信息，供路径浏览功能使用
    * @param mgr  已连接的 SshManager 指针
    * @param cfg  SSH 连接配置
     */
    void setCommunicationInfo(CommunicationManager *manager);

    // 向后兼容接口（槽位 0）
    QString getScriptPath() const;
    void setScriptPath(const QString &path);
    unsigned int getScriptTimeout() const;
    void setScriptTimeout(unsigned int timeoutMs);

private slots:
    void onSaveClicked();
    void onCancelClicked();
    void onBrowseClicked(int slotIndex);
    void onDecreaseSlotCountClicked();
    void onIncreaseSlotCountClicked();

private:
    void buildUi();
    void setupConnections();
    void addSlotTab(int slotIndex);
    void setSlotCount(int newCount);
    void updateSlotCountLabel();

    // 全局配置控件
    QLineEdit   *m_checkIntervalEdit{nullptr};
    QLabel      *m_slotCountValueLabel{nullptr};
    QPushButton *m_decreaseSlotCountButton{nullptr};
    QPushButton *m_increaseSlotCountButton{nullptr};
    QTabWidget  *m_tabWidget{nullptr};
    QPushButton *m_saveButton{nullptr};
    QPushButton *m_cancelButton{nullptr};

    // 按槽位索引缓存控件指针，便于循环操作
    QVector<QLineEdit *>   m_nameEdits;
    QVector<QLineEdit *>   m_pathEdits;
    QVector<QLineEdit *>   m_argsEdits;
    QVector<QLineEdit *>   m_timeoutEdits;
    QVector<QPushButton *> m_browseButtons;

    // 通信信息（由 setCommunicationInfo 注入，供路径浏览使用）
    CommunicationManager *m_commManager { nullptr };
};

// ────────────────────────────────────────────────────────────────
// 通信设置对话框（独立于脚本配置对话框）
// ────────────────────────────────────────────────────────────────
class CommunicationDialog : public QDialog {
    Q_OBJECT
public:
    explicit CommunicationDialog(QWidget *parent = nullptr);
    ~CommunicationDialog() override = default;

    void getCommunicationConfig(CommunicationConfig &cfg) const;
    void setCommunicationConfig(const CommunicationConfig &cfg);
    void saveToSettings();
    void loadFromSettings();

private slots:
    void onSaveClicked();
    void onCancelClicked();
    void onRefreshRs485PortsClicked();

private:
    void buildUi();
    void setupConnections();
    void populateRs485Ports(const QString &preferredPort = QString());
    QVector<int> rs485AllowedBaudRates() const;
    void setRs485AllowedBaudRates(const QVector<int> &baudRates);

    QTabWidget  *m_tabWidget   { nullptr };
    QLineEdit   *m_hostEdit    { nullptr };
    QLineEdit   *m_userEdit    { nullptr };
    QLineEdit   *m_portEdit    { nullptr };
    QLineEdit   *m_passwordEdit{ nullptr };
    QCheckBox   *m_rs485EnabledCheck { nullptr };
    QComboBox   *m_rs485PortCombo { nullptr };
    QComboBox   *m_rs485BaudRateCombo { nullptr };
    QCheckBox   *m_rs485AutoModeCheck { nullptr };
    QLineEdit   *m_rs485AllowedBaudsEdit { nullptr };
    QComboBox   *m_rs485DataBitsCombo { nullptr };
    QComboBox   *m_rs485ParityCombo { nullptr };
    QComboBox   *m_rs485StopBitsCombo { nullptr };
    QComboBox   *m_rs485FlowControlCombo { nullptr };
    QLineEdit   *m_rs485ReconnectIntervalEdit { nullptr };
    QLineEdit   *m_rs485IdleSummaryEdit { nullptr };
    QLineEdit   *m_rs485ProgressIntervalEdit { nullptr };
    QPushButton *m_refreshRs485PortsButton { nullptr };
    QPushButton *m_saveButton  { nullptr };
    QPushButton *m_cancelButton{ nullptr };
};

#endif // CONFIG_DIALOG_H
