#ifndef SSH_FILE_BROWSER_H
#define SSH_FILE_BROWSER_H

#include <QDialog>
#include <QString>
#include <QStringList>

class CommunicationManager;
class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QLabel;
class QPushButton;

/**
 * @class SshFileBrowser
 * @brief 远程文件/目录浏览对话框
 *
 * 用法：
 *   SshFileBrowser dlg(manager, "/opt/robot", parentWidget);
 *   if (dlg.exec() == QDialog::Accepted)
 *       QString path = dlg.selectedPath();
 */
class SshFileBrowser : public QDialog {
    Q_OBJECT

public:
    explicit SshFileBrowser(CommunicationManager *manager,
                            const QString &initialPath = "/",
                            QWidget *parent = nullptr);
    ~SshFileBrowser() override = default;

    /** 对话框 Accepted 后返回用户选定的远程路径 */
    QString selectedPath() const { return m_selectedPath; }

private slots:
    void onNavigateClicked();
    void onParentClicked();
    void onItemDoubleClicked(QListWidgetItem *item);
    void onItemClicked(QListWidgetItem *item);
    void onOkClicked();

private:
    void buildUi();
    void navigateTo(const QString &path);
    QStringList listDirectory(const QString &path, bool &ok, QString *errorMessage = nullptr);
    QString     runRemoteCommand(const QString &remoteCmd, bool &ok,
                                 QString *stderrOutput = nullptr,
                                 int *exitCode = nullptr);

    CommunicationManager *m_commManager { nullptr };
    QString      m_currentPath;
    QString      m_selectedPath;

    QLineEdit   *m_pathEdit       { nullptr };
    QListWidget *m_fileList       { nullptr };
    QLabel      *m_selectedLabel  { nullptr };
    QPushButton *m_okBtn          { nullptr };
};

#endif // SSH_FILE_BROWSER_H
