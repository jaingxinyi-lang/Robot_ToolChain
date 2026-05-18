#include "config_dialog.h"
#include "communication_manager.h"
#include "ssh_file_browser.h"

#include <QComboBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QSettings>
#include <QTabWidget>
#include <QVariant>
#include <QVBoxLayout>

namespace {

int clampSlotCount(int value)
{
    return qBound(SCRIPT_SLOT_MIN, value, SCRIPT_SLOT_MAX);
}

QString formatBaudList(const QVector<int> &baudRates)
{
    QStringList values;
    values.reserve(baudRates.size());
    for (int baudRate : baudRates) {
        if (baudRate > 0) {
            values.push_back(QString::number(baudRate));
        }
    }
    return values.join(",");
}

QVector<int> parseBaudList(const QString &text)
{
    QString normalized = text;
    normalized.replace(QStringLiteral("，"), ",");
    normalized.replace(';', ',');
    normalized.replace(QStringLiteral("；"), ",");
    normalized.replace('\n', ',');
    normalized.replace('\t', ',');
    normalized.replace(' ', ',');

    QVector<int> baudRates;
    const QStringList parts = normalized.split(',', Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        bool ok = false;
        const int baudRate = part.trimmed().toInt(&ok);
        if (ok && baudRate > 0 && !baudRates.contains(baudRate)) {
            baudRates.push_back(baudRate);
        }
    }
    return baudRates;
}

void setComboCurrentData(QComboBox *comboBox, const QVariant &value)
{
    if (!comboBox) {
        return;
    }
    const int index = comboBox->findData(value);
    comboBox->setCurrentIndex(qMax(0, index));
}

} // namespace

ConfigDialog::ConfigDialog(int initialSlotCount, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("脚本配置");
    setModal(true);
    resize(640, 520);

    buildUi();
    setSlotCount(initialSlotCount);

    m_checkIntervalEdit->setValidator(new QIntValidator(100, 10000, this));

    setupConnections();
}

void ConfigDialog::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);

    auto *slotCountLayout = new QHBoxLayout();
    auto *slotCountLabel = new QLabel("脚本槽位数量:", this);
    m_decreaseSlotCountButton = new QPushButton("-", this);
    m_decreaseSlotCountButton->setFixedWidth(32);
    m_slotCountValueLabel = new QLabel(QString::number(SCRIPT_SLOT_DEFAULT), this);
    m_slotCountValueLabel->setMinimumWidth(40);
    m_slotCountValueLabel->setAlignment(Qt::AlignCenter);
    m_slotCountValueLabel->setStyleSheet("QLabel { border: 1px solid #C8CDD5; border-radius: 4px; padding: 4px 8px; background: white; }");
    m_increaseSlotCountButton = new QPushButton("+", this);
    m_increaseSlotCountButton->setFixedWidth(32);

    auto *slotHintLabel = new QLabel(QString("范围 %1-%2，缩小数量会删除超出槽位的配置")
                                         .arg(SCRIPT_SLOT_MIN)
                                         .arg(SCRIPT_SLOT_MAX), this);
    slotHintLabel->setStyleSheet("color: gray;");

    slotCountLayout->addWidget(slotCountLabel);
    slotCountLayout->addWidget(m_decreaseSlotCountButton);
    slotCountLayout->addWidget(m_slotCountValueLabel);
    slotCountLayout->addWidget(m_increaseSlotCountButton);
    slotCountLayout->addWidget(slotHintLabel, 1);
    mainLayout->addLayout(slotCountLayout);

    m_tabWidget = new QTabWidget(this);
    mainLayout->addWidget(m_tabWidget);

    setSlotCount(SCRIPT_SLOT_DEFAULT);

    // ── 全局配置行 ───────────────────────────────────────────────────
    auto *globalForm    = new QFormLayout();
    m_checkIntervalEdit = new QLineEdit("500", this);
    globalForm->addRow("进程检查间隔 (ms):", m_checkIntervalEdit);
    mainLayout->addLayout(globalForm);

    // ── 底部按钮 ─────────────────────────────────────────────────────
    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_saveButton   = new QPushButton("保存", this);
    m_cancelButton = new QPushButton("取消", this);
    m_saveButton->setDefault(true);
    btnLayout->addWidget(m_saveButton);
    btnLayout->addWidget(m_cancelButton);
    mainLayout->addLayout(btnLayout);
}

void ConfigDialog::addSlotTab(int slotIndex)
{
    auto *tab  = new QWidget(m_tabWidget);
    auto *form = new QFormLayout(tab);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(12);

    auto *nameEdit = new QLineEdit(tab);
    nameEdit->setPlaceholderText("无");
    form->addRow("脚本名称:", nameEdit);
    m_nameEdits.push_back(nameEdit);

    auto *pathEdit = new QLineEdit(tab);
    pathEdit->setPlaceholderText("/opt/robot/script.sh  (目标机路径，手动输入或点击浏览)");
    m_pathEdits.push_back(pathEdit);

    auto *browseBtn = new QPushButton("浏览...", tab);
    browseBtn->setFixedWidth(72);
    m_browseButtons.push_back(browseBtn);
    connect(browseBtn, &QPushButton::clicked, this, [this, slotIndex]() {
        onBrowseClicked(slotIndex);
    });

    auto *pathRow = new QHBoxLayout();
    pathRow->setSpacing(6);
    pathRow->addWidget(pathEdit, 1);
    pathRow->addWidget(browseBtn);
    form->addRow("脚本路径:", pathRow);

    auto *argsEdit = new QLineEdit(tab);
    argsEdit->setPlaceholderText("可选，整体作为一个参数传给脚本，例如 127.0.0.1");
    form->addRow("附加参数:", argsEdit);
    m_argsEdits.push_back(argsEdit);

    auto *timeoutEdit = new QLineEdit("0", tab);
    timeoutEdit->setValidator(new QIntValidator(0, 300000, timeoutEdit));
    form->addRow("执行超时 (ms):", timeoutEdit);
    m_timeoutEdits.push_back(timeoutEdit);

    m_tabWidget->addTab(tab, QString("脚本 %1").arg(slotIndex + 1));
}

void ConfigDialog::setSlotCount(int newCount)
{
    const int clampedCount = clampSlotCount(newCount);
    const int currentCount = m_nameEdits.size();
    if (clampedCount == currentCount) {
        updateSlotCountLabel();
        return;
    }

    while (m_nameEdits.size() < clampedCount) {
        addSlotTab(m_nameEdits.size());
    }

    while (m_nameEdits.size() > clampedCount) {
        const int lastIndex = m_nameEdits.size() - 1;
        QWidget *tab = m_tabWidget->widget(lastIndex);
        m_tabWidget->removeTab(lastIndex);
        delete tab;

        m_nameEdits.removeLast();
        m_pathEdits.removeLast();
        m_argsEdits.removeLast();
        m_timeoutEdits.removeLast();
        m_browseButtons.removeLast();
    }

    updateSlotCountLabel();
}

void ConfigDialog::updateSlotCountLabel()
{
    const int slotCount = m_nameEdits.size();
    m_slotCountValueLabel->setText(QString::number(slotCount));
    m_decreaseSlotCountButton->setEnabled(slotCount > SCRIPT_SLOT_MIN);
    m_increaseSlotCountButton->setEnabled(slotCount < SCRIPT_SLOT_MAX);
}

// ---------- 公有接口 ----------

void ConfigDialog::getScriptConfigs(QVector<ScriptConfig> &configs) const
{
    configs.resize(m_nameEdits.size());
    for (int i = 0; i < m_nameEdits.size(); ++i) {
        configs[i].name      = m_nameEdits[i]->text();
        configs[i].path      = m_pathEdits[i]->text();
        configs[i].args      = m_argsEdits[i]->text();
        configs[i].timeoutMs = m_timeoutEdits[i]->text().toUInt();
    }
}

void ConfigDialog::setScriptConfigs(const QVector<ScriptConfig> &configs)
{
    setSlotCount(configs.isEmpty() ? SCRIPT_SLOT_DEFAULT : configs.size());

    for (int i = 0; i < m_nameEdits.size(); ++i) {
        const ScriptConfig config = i < configs.size() ? configs[i] : ScriptConfig{};
        m_nameEdits[i]->setText(config.name);
        m_pathEdits[i]->setText(config.path);
        m_argsEdits[i]->setText(config.args);
        m_timeoutEdits[i]->setText(QString::number(config.timeoutMs));
    }
}

int ConfigDialog::currentSlotCount() const
{
    return m_nameEdits.size();
}

unsigned int ConfigDialog::getProcessCheckInterval() const
{
    return m_checkIntervalEdit->text().toUInt();
}

void ConfigDialog::setProcessCheckInterval(unsigned int intervalMs)
{
    m_checkIntervalEdit->setText(QString::number(intervalMs));
}

QString      ConfigDialog::getScriptPath() const              { return m_pathEdits[0]->text(); }
void         ConfigDialog::setScriptPath(const QString &p)    { m_pathEdits[0]->setText(p); }
unsigned int ConfigDialog::getScriptTimeout() const           { return m_timeoutEdits[0]->text().toUInt(); }
void         ConfigDialog::setScriptTimeout(unsigned int ms)  { m_timeoutEdits[0]->setText(QString::number(ms)); }

// ---------- 私有实现 ----------

void ConfigDialog::setupConnections()
{
    connect(m_saveButton,   &QPushButton::clicked, this, &ConfigDialog::onSaveClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &ConfigDialog::onCancelClicked);
    connect(m_decreaseSlotCountButton, &QPushButton::clicked,
        this, &ConfigDialog::onDecreaseSlotCountClicked);
    connect(m_increaseSlotCountButton, &QPushButton::clicked,
        this, &ConfigDialog::onIncreaseSlotCountClicked);
}

void ConfigDialog::onSaveClicked()
{
    if (m_checkIntervalEdit->text().isEmpty() || m_checkIntervalEdit->text().toUInt() == 0) {
        QMessageBox::warning(this, "错误", "检查间隔必须大于 0");
        return;
    }

    accept();
}

void ConfigDialog::onCancelClicked()
{
    reject();
}

void ConfigDialog::setCommunicationInfo(CommunicationManager *manager)
{
    m_commManager = manager;
}

void ConfigDialog::onBrowseClicked(int slotIndex)
{
    if (!m_commManager || !m_commManager->isConnected()) {
        QMessageBox::warning(this, "通信未连接", "请先建立通信连接，再浏览目标机器目录。");
        return;
    }

    // 确定初始路径：取当前路径输入框中目录部分
    const QString currentText = m_pathEdits[slotIndex]->text().trimmed();
    QString initialPath = "/";
    if (!currentText.isEmpty()) {
        const int slash = currentText.lastIndexOf('/');
        if (slash > 0)
            initialPath = currentText.left(slash);
    }

    SshFileBrowser browser(m_commManager, initialPath, this);
    if (browser.exec() == QDialog::Accepted)
        m_pathEdits[slotIndex]->setText(browser.selectedPath());
}

void ConfigDialog::onDecreaseSlotCountClicked()
{
    setSlotCount(m_nameEdits.size() - 1);
}

void ConfigDialog::onIncreaseSlotCountClicked()
{
    setSlotCount(m_nameEdits.size() + 1);
}

// ────────────────────────────────────────────────────────────────
// CommunicationDialog
// ────────────────────────────────────────────────────────────────

CommunicationDialog::CommunicationDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("通信与 485 配置");
    setModal(true);
    setSizeGripEnabled(true);
    setMinimumSize(680, 520);
    resize(760, 600);
    buildUi();
    m_portEdit->setValidator(new QIntValidator(1, 65535, this));
    m_rs485BaudRateCombo->setValidator(new QIntValidator(1, 5000000, m_rs485BaudRateCombo));
    m_rs485ReconnectIntervalEdit->setValidator(new QIntValidator(1000, 60000, this));
    m_rs485IdleSummaryEdit->setValidator(new QIntValidator(1000, 600000, this));
    m_rs485ProgressIntervalEdit->setValidator(new QIntValidator(0, 1000000, this));
    populateRs485Ports();
    setupConnections();
    loadFromSettings();
}

void CommunicationDialog::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(22, 22, 22, 18);
    mainLayout->setSpacing(16);
    setStyleSheet(R"(
        QDialog { font-size: 14pt; }
        QLabel { font-size: 14pt; }
        QLineEdit, QComboBox { min-height: 34px; padding: 4px 8px; font-size: 14pt; }
        QCheckBox { min-height: 30px; font-size: 14pt; }
        QPushButton { min-height: 34px; padding: 4px 14px; font-size: 14pt; }
    )");

    m_tabWidget = new QTabWidget(this);

    auto *sshPage = new QWidget(m_tabWidget);
    auto *sshForm = new QFormLayout(sshPage);
    sshForm->setContentsMargins(4, 8, 4, 8);
    sshForm->setHorizontalSpacing(18);
    sshForm->setVerticalSpacing(16);
    sshForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_hostEdit = new QLineEdit("192.168.55.102", sshPage);
    m_userEdit = new QLineEdit("noetix", sshPage);
    m_portEdit = new QLineEdit("22", sshPage);
    m_passwordEdit = new QLineEdit(sshPage);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText("请输入密码");
    m_passwordEdit->setText("NoetixBumi@230915!!");
    sshForm->addRow("Host 地址:", m_hostEdit);
    sshForm->addRow("用户名:", m_userEdit);
    sshForm->addRow("端口:", m_portEdit);
    sshForm->addRow("密码:", m_passwordEdit);
    auto *sshHint = new QLabel("提示: 密码以明文存在本地配置文件，仅适用于内网调试工具。", sshPage);
    sshHint->setStyleSheet("color: gray; font-size: 11pt;");
    sshForm->addRow(QString(), sshHint);

    auto *rs485Page = new QWidget(m_tabWidget);
    auto *rs485Form = new QFormLayout(rs485Page);
    rs485Form->setContentsMargins(4, 8, 4, 8);
    rs485Form->setHorizontalSpacing(18);
    rs485Form->setVerticalSpacing(16);
    rs485Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_rs485EnabledCheck = new QCheckBox("随应用启动 485 回环服务", rs485Page);
    m_rs485EnabledCheck->setChecked(true);
    rs485Form->addRow("服务:", m_rs485EnabledCheck);

    m_rs485PortCombo = new QComboBox(rs485Page);
    m_rs485PortCombo->setEditable(true);
    m_refreshRs485PortsButton = new QPushButton("刷新", rs485Page);
    auto *portLayout = new QHBoxLayout();
    portLayout->addWidget(m_rs485PortCombo, 1);
    portLayout->addWidget(m_refreshRs485PortsButton);
    rs485Form->addRow("485 串口:", portLayout);

    m_rs485BaudRateCombo = new QComboBox(rs485Page);
    m_rs485BaudRateCombo->setEditable(true);
    for (int baudRate : {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 1500000}) {
        m_rs485BaudRateCombo->addItem(QString::number(baudRate), baudRate);
    }
    m_rs485BaudRateCombo->setCurrentText("115200");
    rs485Form->addRow("启动波特率:", m_rs485BaudRateCombo);

    m_rs485AutoModeCheck = new QCheckBox("接收控制帧后自动切换波特率", rs485Page);
    m_rs485AutoModeCheck->setChecked(true);
    rs485Form->addRow("AUTO 模式:", m_rs485AutoModeCheck);

    m_rs485AllowedBaudsEdit = new QLineEdit("115200,921600,1000000,1500000", rs485Page);
    rs485Form->addRow("AUTO 白名单:", m_rs485AllowedBaudsEdit);

    m_rs485DataBitsCombo = new QComboBox(rs485Page);
    for (int dataBits : {8, 7, 6, 5}) {
        m_rs485DataBitsCombo->addItem(QString::number(dataBits), dataBits);
    }
    rs485Form->addRow("数据位:", m_rs485DataBitsCombo);

    m_rs485ParityCombo = new QComboBox(rs485Page);
    m_rs485ParityCombo->addItem("无", "none");
    m_rs485ParityCombo->addItem("奇校验", "odd");
    m_rs485ParityCombo->addItem("偶校验", "even");
    m_rs485ParityCombo->addItem("Mark", "mark");
    m_rs485ParityCombo->addItem("Space", "space");
    rs485Form->addRow("校验:", m_rs485ParityCombo);

    m_rs485StopBitsCombo = new QComboBox(rs485Page);
    m_rs485StopBitsCombo->addItem("1", "1");
    m_rs485StopBitsCombo->addItem("1.5", "1.5");
    m_rs485StopBitsCombo->addItem("2", "2");
    rs485Form->addRow("停止位:", m_rs485StopBitsCombo);

    m_rs485FlowControlCombo = new QComboBox(rs485Page);
    m_rs485FlowControlCombo->addItem("无", "none");
    m_rs485FlowControlCombo->addItem("硬件", "hardware");
    m_rs485FlowControlCombo->addItem("软件", "software");
    rs485Form->addRow("流控:", m_rs485FlowControlCombo);

    m_rs485ReconnectIntervalEdit = new QLineEdit("3000", rs485Page);
    rs485Form->addRow("重连间隔 (ms):", m_rs485ReconnectIntervalEdit);

    m_rs485IdleSummaryEdit = new QLineEdit("5000", rs485Page);
    rs485Form->addRow("空闲统计 (ms):", m_rs485IdleSummaryEdit);

    m_rs485ProgressIntervalEdit = new QLineEdit("100", rs485Page);
    rs485Form->addRow("进度日志间隔:", m_rs485ProgressIntervalEdit);

    m_tabWidget->addTab(sshPage, "SSH");
    m_tabWidget->addTab(rs485Page, "485 服务");
    mainLayout->addWidget(m_tabWidget, 1);

    auto *btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(12);
    btnLayout->addStretch();
    m_saveButton   = new QPushButton("保存", this);
    m_cancelButton = new QPushButton("取消", this);
    m_saveButton->setDefault(true);
    btnLayout->addWidget(m_saveButton);
    btnLayout->addWidget(m_cancelButton);
    mainLayout->addLayout(btnLayout);
}

void CommunicationDialog::setupConnections()
{
    connect(m_saveButton,   &QPushButton::clicked, this, &CommunicationDialog::onSaveClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &CommunicationDialog::onCancelClicked);
    connect(m_refreshRs485PortsButton, &QPushButton::clicked,
            this, &CommunicationDialog::onRefreshRs485PortsClicked);
}

void CommunicationDialog::getCommunicationConfig(CommunicationConfig &cfg) const
{
    cfg.ssh.host = m_hostEdit->text().trimmed();
    cfg.ssh.user = m_userEdit->text().trimmed();
    cfg.ssh.port = m_portEdit->text().toInt();
    cfg.ssh.password = m_passwordEdit->text();

    cfg.rs485.enabled = m_rs485EnabledCheck->isChecked();
    const QString selectedPort = m_rs485PortCombo->currentData().toString();
    cfg.rs485.portName = selectedPort.isEmpty()
        ? m_rs485PortCombo->currentText().trimmed()
        : selectedPort;
    cfg.rs485.startBaudRate = m_rs485BaudRateCombo->currentText().trimmed().toInt();
    cfg.rs485.dataBits = m_rs485DataBitsCombo->currentData().toInt();
    cfg.rs485.parity = m_rs485ParityCombo->currentData().toString();
    cfg.rs485.stopBits = m_rs485StopBitsCombo->currentData().toString();
    cfg.rs485.flowControl = m_rs485FlowControlCombo->currentData().toString();
    cfg.rs485.autoModeEnabled = m_rs485AutoModeCheck->isChecked();
    cfg.rs485.allowedBaudRates = rs485AllowedBaudRates();
    cfg.rs485.reconnectIntervalMs = m_rs485ReconnectIntervalEdit->text().toInt();
    cfg.rs485.idleSummaryMs = m_rs485IdleSummaryEdit->text().toInt();
    cfg.rs485.logProgressInterval = m_rs485ProgressIntervalEdit->text().toInt();
}

void CommunicationDialog::setCommunicationConfig(const CommunicationConfig &cfg)
{
    m_hostEdit->setText(cfg.ssh.host);
    m_userEdit->setText(cfg.ssh.user);
    m_portEdit->setText(QString::number(cfg.ssh.port));
    m_passwordEdit->setText(cfg.ssh.password);

    m_rs485EnabledCheck->setChecked(cfg.rs485.enabled);
    populateRs485Ports(cfg.rs485.portName);
    int baudRateIndex = m_rs485BaudRateCombo->findData(cfg.rs485.startBaudRate);
    if (baudRateIndex < 0) {
        m_rs485BaudRateCombo->addItem(QString::number(cfg.rs485.startBaudRate), cfg.rs485.startBaudRate);
        baudRateIndex = m_rs485BaudRateCombo->findData(cfg.rs485.startBaudRate);
    }
    m_rs485BaudRateCombo->setCurrentIndex(qMax(0, baudRateIndex));
    m_rs485AutoModeCheck->setChecked(cfg.rs485.autoModeEnabled);
    setRs485AllowedBaudRates(cfg.rs485.allowedBaudRates);
    setComboCurrentData(m_rs485DataBitsCombo, cfg.rs485.dataBits);
    setComboCurrentData(m_rs485ParityCombo, cfg.rs485.parity);
    setComboCurrentData(m_rs485StopBitsCombo, cfg.rs485.stopBits);
    setComboCurrentData(m_rs485FlowControlCombo, cfg.rs485.flowControl);
    m_rs485ReconnectIntervalEdit->setText(QString::number(cfg.rs485.reconnectIntervalMs));
    m_rs485IdleSummaryEdit->setText(QString::number(cfg.rs485.idleSummaryMs));
    m_rs485ProgressIntervalEdit->setText(QString::number(cfg.rs485.logProgressInterval));
}

void CommunicationDialog::saveToSettings()
{
    CommunicationConfig cfg;
    getCommunicationConfig(cfg);

    QSettings settings("RobotTeam", "RobotToolChain");
    settings.beginGroup("SSH");
    settings.setValue("host", cfg.ssh.host);
    settings.setValue("user", cfg.ssh.user);
    settings.setValue("port", cfg.ssh.port);
    settings.setValue("password", cfg.ssh.password);
    settings.endGroup();

    settings.beginGroup("RS485Service");
    settings.setValue("enabled", cfg.rs485.enabled);
    settings.setValue("portName", cfg.rs485.portName);
    settings.setValue("startBaudRate", cfg.rs485.startBaudRate);
    settings.setValue("dataBits", cfg.rs485.dataBits);
    settings.setValue("parity", cfg.rs485.parity);
    settings.setValue("stopBits", cfg.rs485.stopBits);
    settings.setValue("flowControl", cfg.rs485.flowControl);
    settings.setValue("autoModeEnabled", cfg.rs485.autoModeEnabled);
    settings.setValue("allowedBaudRates", formatBaudList(cfg.rs485.allowedBaudRates));
    settings.setValue("reconnectIntervalMs", cfg.rs485.reconnectIntervalMs);
    settings.setValue("idleSummaryMs", cfg.rs485.idleSummaryMs);
    settings.setValue("logProgressInterval", cfg.rs485.logProgressInterval);
    settings.endGroup();
    settings.sync();
}

void CommunicationDialog::loadFromSettings()
{
    CommunicationConfig cfg;

    QSettings settings("RobotTeam", "RobotToolChain");
    settings.beginGroup("SSH");
    cfg.ssh.host = settings.value("host", cfg.ssh.host).toString();
    cfg.ssh.user = settings.value("user", cfg.ssh.user).toString();
    cfg.ssh.port = settings.value("port", cfg.ssh.port).toInt();
    cfg.ssh.password = settings.value("password", cfg.ssh.password).toString();
    settings.endGroup();

    bool hasRs485Settings = false;
    settings.beginGroup("RS485Service");
    hasRs485Settings = !settings.childKeys().isEmpty();
    cfg.rs485.enabled = settings.value("enabled", cfg.rs485.enabled).toBool();
    cfg.rs485.portName = settings.value("portName", cfg.rs485.portName).toString();
    cfg.rs485.startBaudRate = settings.value("startBaudRate", cfg.rs485.startBaudRate).toInt();
    cfg.rs485.dataBits = settings.value("dataBits", cfg.rs485.dataBits).toInt();
    cfg.rs485.parity = settings.value("parity", cfg.rs485.parity).toString();
    cfg.rs485.stopBits = settings.value("stopBits", cfg.rs485.stopBits).toString();
    cfg.rs485.flowControl = settings.value("flowControl", cfg.rs485.flowControl).toString();
    cfg.rs485.autoModeEnabled = settings.value("autoModeEnabled", cfg.rs485.autoModeEnabled).toBool();
    cfg.rs485.allowedBaudRates = parseBaudList(
        settings.value("allowedBaudRates", formatBaudList(cfg.rs485.allowedBaudRates)).toString());
    cfg.rs485.reconnectIntervalMs = settings.value("reconnectIntervalMs", cfg.rs485.reconnectIntervalMs).toInt();
    cfg.rs485.idleSummaryMs = settings.value("idleSummaryMs", cfg.rs485.idleSummaryMs).toInt();
    cfg.rs485.logProgressInterval = settings.value("logProgressInterval", cfg.rs485.logProgressInterval).toInt();
    settings.endGroup();

    if (!hasRs485Settings) {
        settings.beginGroup("Serial");
        cfg.rs485.portName = settings.value("portName", cfg.rs485.portName).toString();
        cfg.rs485.startBaudRate = settings.value("baudRate", cfg.rs485.startBaudRate).toInt();
        cfg.rs485.dataBits = settings.value("dataBits", cfg.rs485.dataBits).toInt();
        cfg.rs485.parity = settings.value("parity", cfg.rs485.parity).toString();
        cfg.rs485.stopBits = settings.value("stopBits", cfg.rs485.stopBits).toString();
        cfg.rs485.flowControl = settings.value("flowControl", cfg.rs485.flowControl).toString();
        settings.endGroup();
    }

    if (cfg.rs485.allowedBaudRates.isEmpty()) {
        cfg.rs485.allowedBaudRates = Rs485Config{}.allowedBaudRates;
    }

    setCommunicationConfig(cfg);
}

void CommunicationDialog::onSaveClicked()
{
    CommunicationConfig cfg;
    getCommunicationConfig(cfg);

    if (cfg.ssh.host.isEmpty() || cfg.ssh.user.isEmpty() || cfg.ssh.port <= 0) {
        QMessageBox::warning(this, "错误", "请填写完整的 SSH 地址、用户名和端口。");
        return;
    }

    if (cfg.rs485.enabled) {
        if (cfg.rs485.portName.isEmpty()) {
            QMessageBox::warning(this, "错误", "请选择或输入 485 串口名称。");
            return;
        }
        if (cfg.rs485.startBaudRate <= 0) {
            QMessageBox::warning(this, "错误", "485 启动波特率必须大于 0。");
            return;
        }
        if (cfg.rs485.autoModeEnabled && cfg.rs485.allowedBaudRates.isEmpty()) {
            QMessageBox::warning(this, "错误", "AUTO 模式至少需要一个波特率白名单值。");
            return;
        }
        if (cfg.rs485.reconnectIntervalMs < 1000) {
            QMessageBox::warning(this, "错误", "485 重连间隔不能小于 1000ms。");
            return;
        }
        if (cfg.rs485.idleSummaryMs < 1000) {
            QMessageBox::warning(this, "错误", "485 空闲统计间隔不能小于 1000ms。");
            return;
        }
        if (cfg.rs485.logProgressInterval < 0) {
            QMessageBox::warning(this, "错误", "485 进度日志间隔不能小于 0。");
            return;
        }
    }

    saveToSettings();
    accept();
}

void CommunicationDialog::onCancelClicked()
{
    reject();
}

void CommunicationDialog::onRefreshRs485PortsClicked()
{
    populateRs485Ports(m_rs485PortCombo->currentText());
}

void CommunicationDialog::populateRs485Ports(const QString &preferredPort)
{
    const QString previous = preferredPort.isEmpty() && m_rs485PortCombo
        ? m_rs485PortCombo->currentText()
        : preferredPort;
    m_rs485PortCombo->clear();

    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
        const QString label = info.description().isEmpty()
            ? info.portName()
            : QString("%1 (%2)").arg(info.portName(), info.description());
        m_rs485PortCombo->addItem(label, info.portName());
    }

    if (!previous.isEmpty() && m_rs485PortCombo->findText(previous) < 0
        && m_rs485PortCombo->findData(previous) < 0) {
        m_rs485PortCombo->addItem(previous, previous);
    }

    int index = previous.isEmpty() ? 0 : m_rs485PortCombo->findData(previous);
    if (index < 0) {
        index = m_rs485PortCombo->findText(previous);
    }
    if (index >= 0) {
        m_rs485PortCombo->setCurrentIndex(index);
    }
}

QVector<int> CommunicationDialog::rs485AllowedBaudRates() const
{
    return parseBaudList(m_rs485AllowedBaudsEdit->text());
}

void CommunicationDialog::setRs485AllowedBaudRates(const QVector<int> &baudRates)
{
    m_rs485AllowedBaudsEdit->setText(formatBaudList(baudRates));
}

