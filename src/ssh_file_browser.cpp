#include "ssh_file_browser.h"
#include "communication_manager.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QString buildDirectoryErrorMessage(const QString &path, const QString &details, int exitCode)
{
    const QString trimmed = details.trimmed();

    if (trimmed.contains("Permission denied", Qt::CaseInsensitive)) {
        return QString("无法列出目录:\n%1\n\n远程主机返回：权限不足。")
            .arg(path);
    }

    if (trimmed.contains("No such file", Qt::CaseInsensitive)
        || trimmed.contains("cannot access", Qt::CaseInsensitive)) {
        return QString("无法列出目录:\n%1\n\n远程主机返回：路径不存在或不可访问。")
            .arg(path);
    }

    if (!trimmed.isEmpty()) {
        return QString("无法列出目录:\n%1\n\n远程主机返回:\n%2")
            .arg(path, trimmed);
    }

    return QString("无法列出目录:\n%1\n\n命令执行失败，退出码: %2。")
        .arg(path)
        .arg(exitCode);
}

} // namespace

// ── 构造 ──────────────────────────────────────────────────────────────────────

SshFileBrowser::SshFileBrowser(CommunicationManager *manager,
                               const QString &initialPath,
                               QWidget *parent)
    : QDialog(parent)
    , m_commManager(manager)
    , m_currentPath(initialPath.isEmpty() ? "/" : initialPath)
{
    setWindowTitle(QString("远程文件浏览  —  %1").arg(manager ? manager->modeName() : "未连接"));
    setMinimumSize(520, 460);
    resize(580, 520);
    buildUi();
    navigateTo(m_currentPath);
}

// ── UI 构建 ───────────────────────────────────────────────────────────────────

void SshFileBrowser::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);
    mainLayout->setContentsMargins(10, 10, 10, 10);

    // ── 路径栏 ────────────────────────────────────────────────────────────────
    auto *pathBar   = new QHBoxLayout();
    auto *pathLabel = new QLabel("路径:", this);
    pathLabel->setFixedWidth(36);

    m_pathEdit = new QLineEdit(m_currentPath, this);
    m_pathEdit->setPlaceholderText("/");

    auto *goBtn = new QPushButton("前往", this);
    goBtn->setFixedWidth(58);

    auto *parentBtn = new QPushButton("↑ 上级", this);
    parentBtn->setFixedWidth(66);

    pathBar->addWidget(pathLabel);
    pathBar->addWidget(m_pathEdit, 1);
    pathBar->addWidget(goBtn);
    pathBar->addWidget(parentBtn);
    mainLayout->addLayout(pathBar);

    // ── 文件/目录列表 ─────────────────────────────────────────────────────────
    m_fileList = new QListWidget(this);
    m_fileList->setFont(QFont("Monospace", 11));
    m_fileList->setAlternatingRowColors(true);
    mainLayout->addWidget(m_fileList, 1);

    // ── 当前选择提示 ──────────────────────────────────────────────────────────
    m_selectedLabel = new QLabel("已选择: (无)", this);
    m_selectedLabel->setStyleSheet("color: #555; font-size: 10pt;");
    m_selectedLabel->setWordWrap(true);
    mainLayout->addWidget(m_selectedLabel);

    // ── 底部按钮 ──────────────────────────────────────────────────────────────
    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_okBtn          = new QPushButton("确定", this);
    auto *cancelBtn  = new QPushButton("取消", this);
    m_okBtn->setDefault(true);
    m_okBtn->setEnabled(false);
    btnLayout->addWidget(m_okBtn);
    btnLayout->addWidget(cancelBtn);
    mainLayout->addLayout(btnLayout);

    // ── 信号槽 ────────────────────────────────────────────────────────────────
    connect(goBtn,      &QPushButton::clicked,
            this, &SshFileBrowser::onNavigateClicked);
    connect(m_pathEdit, &QLineEdit::returnPressed,
            this, &SshFileBrowser::onNavigateClicked);
    connect(parentBtn,  &QPushButton::clicked,
            this, &SshFileBrowser::onParentClicked);
    connect(m_fileList, &QListWidget::itemDoubleClicked,
            this, &SshFileBrowser::onItemDoubleClicked);
    connect(m_fileList, &QListWidget::itemClicked,
            this, &SshFileBrowser::onItemClicked);
    connect(m_okBtn,    &QPushButton::clicked,
            this, &SshFileBrowser::onOkClicked);
    connect(cancelBtn,  &QPushButton::clicked,
            this, &QDialog::reject);
}

// ── 目录导航 ──────────────────────────────────────────────────────────────────

/**
 * @brief 切换到指定远程路径并刷新列表
 */
void SshFileBrowser::navigateTo(const QString &path)
{
    // 规范化：去掉尾部多余的 '/'（根目录除外）
    QString normalized = path.trimmed().isEmpty() ? "/" : path.trimmed();
    if (normalized.length() > 1 && normalized.endsWith('/'))
        normalized.chop(1);

    bool ok = false;
    QString errorMessage;
    const QStringList entries = listDirectory(normalized, ok, &errorMessage);

    if (!ok) {
        QMessageBox::warning(this, "访问失败",
            errorMessage.isEmpty()
                ? QString("无法列出目录:\n%1\n\n请确认路径存在且 SSH 连接正常。").arg(normalized)
                : errorMessage);
        // 恢复路径栏为上次成功的路径
        m_pathEdit->setText(m_currentPath);
        return;
    }

    m_currentPath = normalized;
    m_pathEdit->setText(m_currentPath);
    m_fileList->clear();
    m_selectedPath.clear();
    m_selectedLabel->setText("已选择: (无)");
    m_okBtn->setEnabled(false);

    // 始终在顶部添加 ".." 条目（根目录除外）
    if (m_currentPath != "/") {
        auto *upItem = new QListWidgetItem("📁   ..", m_fileList);
        upItem->setData(Qt::UserRole,     QString("..PARENT.."));
        upItem->setData(Qt::UserRole + 1, true);
        QFont f = upItem->font();
        f.setItalic(true);
        upItem->setFont(f);
    }

    // 解析 ls 输出（ls -1F 格式，目录以 '/' 结尾，可执行文件以 '*' 结尾）
    // 按目录 / 文件分类后合并显示
    QStringList dirs, files;
    for (const QString &raw : entries) {
        const QString entry = raw.trimmed();
        if (entry.isEmpty()) continue;

        bool isDir = entry.endsWith('/');

        // 提取纯名称（剥去类型标记 / * @ | = >）
        QString name = entry;
        if (!name.isEmpty()) {
            const QChar last = name.back();
            static const QString indicators = "/*@|=>";
            if (indicators.contains(last))
                name.chop(1);
        }
        if (name.isEmpty() || name == "." || name == "..") continue;

        if (isDir)
            dirs << name;
        else
            files << name;
    }
    dirs.sort(Qt::CaseInsensitive);
    files.sort(Qt::CaseInsensitive);

    for (const QString &name : dirs) {
        auto *item = new QListWidgetItem(QString("📁   %1").arg(name), m_fileList);
        item->setData(Qt::UserRole,     name + "/");
        item->setData(Qt::UserRole + 1, true);
    }
    for (const QString &name : files) {
        auto *item = new QListWidgetItem(QString("📄   %1").arg(name), m_fileList);
        item->setData(Qt::UserRole,     name);
        item->setData(Qt::UserRole + 1, false);
    }
}

// ── 远程目录列举 ──────────────────────────────────────────────────────────────

QStringList SshFileBrowser::listDirectory(const QString &path, bool &ok, QString *errorMessage)
{
    // 对路径中的单引号做转义，防止命令注入
    QString safePath = path;
    safePath.replace("'", "'\\''");

    // ls -1F: 每行一个条目，目录以 '/'、可执行以 '*' 结尾
    const QString cmd = QString("LC_ALL=C ls -1F -- '%1'").arg(safePath);

    QString stderrOutput;
    int exitCode = -1;
    const QString output = runRemoteCommand(cmd, ok, &stderrOutput, &exitCode);

    if (!ok) {
        if (errorMessage) {
            *errorMessage = buildDirectoryErrorMessage(path, stderrOutput, exitCode);
        }
        return {};
    }

    if (exitCode != 0) {
        ok = false;
        if (errorMessage) {
            *errorMessage = buildDirectoryErrorMessage(path, stderrOutput, exitCode);
        }
        return {};
    }

    return output.split('\n', Qt::SkipEmptyParts);
}

// ── 远程命令执行 ──────────────────────────────────────────────────────────────

/**
 * @brief 在远程主机上执行命令并返回标准输出
 *
 * 通过已连接的通信管理器执行命令。
 * 若通信未连接则立即返回失败。
 */
QString SshFileBrowser::runRemoteCommand(const QString &remoteCmd, bool &ok,
                                         QString *stderrOutput,
                                         int *exitCode)
{
    if (!m_commManager || !m_commManager->isConnected()) {
        if (stderrOutput) {
            *stderrOutput = "通信未连接";
        }
        if (exitCode) {
            *exitCode = -1;
        }
        ok = false;
        return {};
    }

    QString output;
    ok = m_commManager->executeCommand(remoteCmd, output, stderrOutput, exitCode, 30000);
    return output;
}

// ── 槽函数 ───────────────────────────────────────────────────────────────────

void SshFileBrowser::onNavigateClicked()
{
    navigateTo(m_pathEdit->text().trimmed());
}

void SshFileBrowser::onParentClicked()
{
    if (m_currentPath == "/") return;
    const int slash = m_currentPath.lastIndexOf('/');
    navigateTo(slash > 0 ? m_currentPath.left(slash) : "/");
}

void SshFileBrowser::onItemDoubleClicked(QListWidgetItem *item)
{
    if (!item) return;

    const QString data  = item->data(Qt::UserRole).toString();
    const bool    isDir = item->data(Qt::UserRole + 1).toBool();

    if (data == "..PARENT..") {
        onParentClicked();
        return;
    }

    if (isDir) {
        // 进入子目录（剥去尾部 '/'）
        const QString sub = data.left(data.length() - 1);
        navigateTo(m_currentPath == "/" ? "/" + sub
                                        : m_currentPath + "/" + sub);
    } else {
        // 双击文件 → 直接确认
        m_selectedPath = (m_currentPath == "/" ? "/" : m_currentPath + "/") + data;
        accept();
    }
}

void SshFileBrowser::onItemClicked(QListWidgetItem *item)
{
    if (!item) return;

    const QString data  = item->data(Qt::UserRole).toString();
    const bool    isDir = item->data(Qt::UserRole + 1).toBool();

    if (data == "..PARENT..") {
        m_selectedPath.clear();
        m_selectedLabel->setText("已选择: (无)");
        m_okBtn->setEnabled(false);
        return;
    }

    // 目录本身也可被选为路径（去掉尾部 '/'）
    const QString name = isDir ? data.left(data.length() - 1) : data;
    m_selectedPath = (m_currentPath == "/" ? "/" : m_currentPath + "/") + name;
    m_selectedLabel->setText(QString("已选择: %1").arg(m_selectedPath));
    m_okBtn->setEnabled(true);
}

void SshFileBrowser::onOkClicked()
{
    if (!m_selectedPath.isEmpty())
        accept();
}
