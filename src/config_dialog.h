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
    Ssh,
    Serial
};

struct SerialConfig {
    QString portName;
    int baudRate { 1500000 };
    int dataBits { 8 };
    QString parity { "none" };
    QString stopBits { "1" };
    QString flowControl { "none" };
    int commandTimeoutMs { 300000 };
};

struct CommunicationConfig {
    CommunicationMode mode { CommunicationMode::Serial };
    SshConfig ssh;
    SerialConfig serial;
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
    void onRefreshSerialPortsClicked();

private:
    void buildUi();
    void setupConnections();
    void populateSerialPorts(const QString &preferredPort = QString());
    CommunicationMode selectedMode() const;
    void setSelectedMode(CommunicationMode mode);

    QComboBox   *m_modeCombo       { nullptr };
    QStackedWidget *m_stack        { nullptr };
    QLineEdit   *m_hostEdit    { nullptr };
    QLineEdit   *m_userEdit    { nullptr };
    QLineEdit   *m_portEdit    { nullptr };
    QLineEdit   *m_passwordEdit{ nullptr };
    QComboBox   *m_serialPortCombo { nullptr };
    QComboBox   *m_baudRateCombo   { nullptr };
    QComboBox   *m_dataBitsCombo   { nullptr };
    QComboBox   *m_parityCombo     { nullptr };
    QComboBox   *m_stopBitsCombo   { nullptr };
    QComboBox   *m_flowControlCombo{ nullptr };
    QLineEdit   *m_commandTimeoutEdit { nullptr };
    QPushButton *m_refreshPortsButton { nullptr };
    QPushButton *m_saveButton  { nullptr };
    QPushButton *m_cancelButton{ nullptr };
};

#endif // CONFIG_DIALOG_H
