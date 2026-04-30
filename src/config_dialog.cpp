#include "config_dialog.h"
#include "communication_manager.h"
#include "ssh_file_browser.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QSettings>
#include <QStackedWidget>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

int clampSlotCount(int value)
{
    return qBound(SCRIPT_SLOT_MIN, value, SCRIPT_SLOT_MAX);
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
    setWindowTitle("通信配置");
    setModal(true);
    setSizeGripEnabled(true);
    setMinimumSize(680, 520);
    resize(760, 600);
    buildUi();
    m_portEdit->setValidator(new QIntValidator(1, 65535, this));
    m_baudRateCombo->setValidator(new QIntValidator(1, 5000000, m_baudRateCombo));
    m_commandTimeoutEdit->setValidator(new QIntValidator(1000, 3600000, this));
    populateSerialPorts();
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
        QPushButton { min-height: 34px; padding: 4px 14px; font-size: 14pt; }
    )");

    auto *modeLayout = new QHBoxLayout();
    modeLayout->setSpacing(14);
    modeLayout->addWidget(new QLabel("通信方式:", this));
    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItem("SSH", "ssh");
    m_modeCombo->addItem("串口", "serial");
    modeLayout->addWidget(m_modeCombo, 1);
    mainLayout->addLayout(modeLayout);

    m_stack = new QStackedWidget(this);

    auto *sshPage = new QWidget(m_stack);
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

    auto *serialPage = new QWidget(m_stack);
    auto *serialForm = new QFormLayout(serialPage);
    serialForm->setContentsMargins(4, 8, 4, 8);
    serialForm->setHorizontalSpacing(18);
    serialForm->setVerticalSpacing(16);
    serialForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_serialPortCombo = new QComboBox(serialPage);
    m_serialPortCombo->setEditable(true);
    m_refreshPortsButton = new QPushButton("刷新", serialPage);
    auto *portLayout = new QHBoxLayout();
    portLayout->addWidget(m_serialPortCombo, 1);
    portLayout->addWidget(m_refreshPortsButton);
    serialForm->addRow("串口:", portLayout);

    m_baudRateCombo = new QComboBox(serialPage);
    m_baudRateCombo->setEditable(true);
    for (int baudRate : {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1500000}) {
        m_baudRateCombo->addItem(QString::number(baudRate), baudRate);
    }
    m_baudRateCombo->setCurrentText("1500000");
    serialForm->addRow("波特率:", m_baudRateCombo);

    m_dataBitsCombo = new QComboBox(serialPage);
    for (int dataBits : {8, 7, 6, 5}) {
        m_dataBitsCombo->addItem(QString::number(dataBits), dataBits);
    }
    serialForm->addRow("数据位:", m_dataBitsCombo);

    m_parityCombo = new QComboBox(serialPage);
    m_parityCombo->addItem("无", "none");
    m_parityCombo->addItem("奇校验", "odd");
    m_parityCombo->addItem("偶校验", "even");
    m_parityCombo->addItem("Mark", "mark");
    m_parityCombo->addItem("Space", "space");
    serialForm->addRow("校验:", m_parityCombo);

    m_stopBitsCombo = new QComboBox(serialPage);
    m_stopBitsCombo->addItem("1", "1");
    m_stopBitsCombo->addItem("1.5", "1.5");
    m_stopBitsCombo->addItem("2", "2");
    serialForm->addRow("停止位:", m_stopBitsCombo);

    m_flowControlCombo = new QComboBox(serialPage);
    m_flowControlCombo->addItem("无", "none");
    m_flowControlCombo->addItem("硬件", "hardware");
    m_flowControlCombo->addItem("软件", "software");
    serialForm->addRow("流控:", m_flowControlCombo);

    m_commandTimeoutEdit = new QLineEdit("300000", serialPage);
    serialForm->addRow("命令超时 (ms):", m_commandTimeoutEdit);

    m_stack->addWidget(sshPage);
    m_stack->addWidget(serialPage);
    mainLayout->addWidget(m_stack, 1);

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
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            m_stack, &QStackedWidget::setCurrentIndex);
    connect(m_saveButton,   &QPushButton::clicked, this, &CommunicationDialog::onSaveClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &CommunicationDialog::onCancelClicked);
    connect(m_refreshPortsButton, &QPushButton::clicked,
            this, &CommunicationDialog::onRefreshSerialPortsClicked);
}

void CommunicationDialog::getCommunicationConfig(CommunicationConfig &cfg) const
{
    cfg.mode = selectedMode();
    cfg.ssh.host = m_hostEdit->text().trimmed();
    cfg.ssh.user = m_userEdit->text().trimmed();
    cfg.ssh.port = m_portEdit->text().toInt();
    cfg.ssh.password = m_passwordEdit->text();

    const QString selectedPort = m_serialPortCombo->currentData().toString();
    cfg.serial.portName = selectedPort.isEmpty()
        ? m_serialPortCombo->currentText().trimmed()
        : selectedPort;
    cfg.serial.baudRate = m_baudRateCombo->currentText().trimmed().toInt();
    cfg.serial.dataBits = m_dataBitsCombo->currentData().toInt();
    cfg.serial.parity = m_parityCombo->currentData().toString();
    cfg.serial.stopBits = m_stopBitsCombo->currentData().toString();
    cfg.serial.flowControl = m_flowControlCombo->currentData().toString();
    cfg.serial.commandTimeoutMs = m_commandTimeoutEdit->text().toInt();
}

void CommunicationDialog::setCommunicationConfig(const CommunicationConfig &cfg)
{
    setSelectedMode(cfg.mode);

    m_hostEdit->setText(cfg.ssh.host);
    m_userEdit->setText(cfg.ssh.user);
    m_portEdit->setText(QString::number(cfg.ssh.port));
    m_passwordEdit->setText(cfg.ssh.password);

    populateSerialPorts(cfg.serial.portName);
    int baudRateIndex = m_baudRateCombo->findData(cfg.serial.baudRate);
    if (baudRateIndex < 0) {
        m_baudRateCombo->addItem(QString::number(cfg.serial.baudRate), cfg.serial.baudRate);
        baudRateIndex = m_baudRateCombo->findData(cfg.serial.baudRate);
    }
    m_baudRateCombo->setCurrentIndex(qMax(0, baudRateIndex));
    m_dataBitsCombo->setCurrentIndex(qMax(0, m_dataBitsCombo->findData(cfg.serial.dataBits)));
    m_parityCombo->setCurrentIndex(qMax(0, m_parityCombo->findData(cfg.serial.parity)));
    m_stopBitsCombo->setCurrentIndex(qMax(0, m_stopBitsCombo->findData(cfg.serial.stopBits)));
    m_flowControlCombo->setCurrentIndex(qMax(0, m_flowControlCombo->findData(cfg.serial.flowControl)));
    m_commandTimeoutEdit->setText(QString::number(cfg.serial.commandTimeoutMs));
}

void CommunicationDialog::saveToSettings()
{
    CommunicationConfig cfg;
    getCommunicationConfig(cfg);

    QSettings settings("RobotTeam", "RobotToolChain");
    settings.beginGroup("Communication");
    settings.setValue("mode", cfg.mode == CommunicationMode::Serial ? "serial" : "ssh");
    settings.endGroup();

    settings.beginGroup("SSH");
    settings.setValue("host", cfg.ssh.host);
    settings.setValue("user", cfg.ssh.user);
    settings.setValue("port", cfg.ssh.port);
    settings.setValue("password", cfg.ssh.password);
    settings.endGroup();

    settings.beginGroup("Serial");
    settings.setValue("portName", cfg.serial.portName);
    settings.setValue("baudRate", cfg.serial.baudRate);
    settings.setValue("dataBits", cfg.serial.dataBits);
    settings.setValue("parity", cfg.serial.parity);
    settings.setValue("stopBits", cfg.serial.stopBits);
    settings.setValue("flowControl", cfg.serial.flowControl);
    settings.setValue("commandTimeoutMs", cfg.serial.commandTimeoutMs);
    settings.endGroup();
    settings.sync();
}

void CommunicationDialog::loadFromSettings()
{
    CommunicationConfig cfg;

    QSettings settings("RobotTeam", "RobotToolChain");
    settings.beginGroup("Communication");
    cfg.mode = settings.value("mode", "serial").toString().toLower() == "serial"
        ? CommunicationMode::Serial
        : CommunicationMode::Ssh;
    settings.endGroup();

    settings.beginGroup("SSH");
    cfg.ssh.host = settings.value("host", cfg.ssh.host).toString();
    cfg.ssh.user = settings.value("user", cfg.ssh.user).toString();
    cfg.ssh.port = settings.value("port", cfg.ssh.port).toInt();
    cfg.ssh.password = settings.value("password", cfg.ssh.password).toString();
    settings.endGroup();

    settings.beginGroup("Serial");
    cfg.serial.portName = settings.value("portName", cfg.serial.portName).toString();
    cfg.serial.baudRate = settings.value("baudRate", cfg.serial.baudRate).toInt();
    cfg.serial.dataBits = settings.value("dataBits", cfg.serial.dataBits).toInt();
    cfg.serial.parity = settings.value("parity", cfg.serial.parity).toString();
    cfg.serial.stopBits = settings.value("stopBits", cfg.serial.stopBits).toString();
    cfg.serial.flowControl = settings.value("flowControl", cfg.serial.flowControl).toString();
    cfg.serial.commandTimeoutMs = settings.value("commandTimeoutMs", cfg.serial.commandTimeoutMs).toInt();
    settings.endGroup();

    setCommunicationConfig(cfg);
}

void CommunicationDialog::onSaveClicked()
{
    CommunicationConfig cfg;
    getCommunicationConfig(cfg);

    if (cfg.mode == CommunicationMode::Ssh) {
        if (cfg.ssh.host.isEmpty() || cfg.ssh.user.isEmpty() || cfg.ssh.port <= 0) {
            QMessageBox::warning(this, "错误", "请填写完整的 SSH 地址、用户名和端口。");
            return;
        }
    } else {
        if (cfg.serial.portName.isEmpty()) {
            QMessageBox::warning(this, "错误", "请选择或输入串口名称。");
            return;
        }
        if (cfg.serial.baudRate <= 0) {
            QMessageBox::warning(this, "错误", "波特率必须大于 0。");
            return;
        }
        if (cfg.serial.commandTimeoutMs <= 0) {
            QMessageBox::warning(this, "错误", "串口命令超时必须大于 0。");
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

void CommunicationDialog::onRefreshSerialPortsClicked()
{
    populateSerialPorts(m_serialPortCombo->currentText());
}

void CommunicationDialog::populateSerialPorts(const QString &preferredPort)
{
    const QString previous = preferredPort.isEmpty() && m_serialPortCombo
        ? m_serialPortCombo->currentText()
        : preferredPort;
    m_serialPortCombo->clear();

    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
        const QString label = info.description().isEmpty()
            ? info.portName()
            : QString("%1 (%2)").arg(info.portName(), info.description());
        m_serialPortCombo->addItem(label, info.portName());
    }

    if (!previous.isEmpty() && m_serialPortCombo->findText(previous) < 0
        && m_serialPortCombo->findData(previous) < 0) {
        m_serialPortCombo->addItem(previous, previous);
    }

    int index = previous.isEmpty() ? 0 : m_serialPortCombo->findData(previous);
    if (index < 0) {
        index = m_serialPortCombo->findText(previous);
    }
    if (index >= 0) {
        m_serialPortCombo->setCurrentIndex(index);
    }
}

CommunicationMode CommunicationDialog::selectedMode() const
{
    return m_modeCombo->currentData().toString() == "serial"
        ? CommunicationMode::Serial
        : CommunicationMode::Ssh;
}

void CommunicationDialog::setSelectedMode(CommunicationMode mode)
{
    const QString value = mode == CommunicationMode::Serial ? "serial" : "ssh";
    const int index = m_modeCombo->findData(value);
    if (index >= 0) {
        m_modeCombo->setCurrentIndex(index);
        m_stack->setCurrentIndex(index);
    }
}

