#include "file_deployer.h"

#include <QCoreApplication>
#include <QFileInfo>

namespace {

QString remoteDeployDir()
{
    return QStringLiteral("/home/noetix/JIAOBEN");
}

QString remotePackagePath()
{
    return QStringLiteral("/tmp/JIAOBEN.zip");
}

} // namespace

FileDeployer::FileDeployer(CommunicationManager *communicationManager,
                           QObject *parent)
    : QObject(parent)
    , m_commManager(communicationManager)
{
}

bool FileDeployer::deployPackage(const QString &packagePath)
{
    m_cancelRequested = false;

    if (!m_commManager || !m_commManager->isConnected()) {
        emit error("请先建立通信连接，再部署脚本包");
        return false;
    }

    if (!ensurePackageReadable(packagePath)) {
        return false;
    }

    emit info(QString("开始部署脚本包：%1").arg(packagePath));
    processUiEvents();

    return deployOverSsh(packagePath);
}

void FileDeployer::cancel()
{
    m_cancelRequested = true;
}

bool FileDeployer::deployOverSsh(const QString &packagePath)
{
    const QString deployDir = remoteDeployDir();
    const QString packageTempPath = remotePackagePath();

    if (!runRemoteCommand(QString("检查开发板解压工具与用户目录写权限：%1 ...").arg(deployDir),
                          QString("command -v unzip >/dev/null 2>&1 && mkdir -p %1 && test -w %1")
                              .arg(deployDir),
                          30000,
                          false)) {
        return false;
    }

    emit info(QString("通过 SSH/SFTP 上传 JIAOBEN.zip 到 %1 ...").arg(packageTempPath));
    processUiEvents();

    QString uploadError;
    int lastPercent = -1;
    const bool uploaded = m_commManager->sshManager()->uploadFile(
        packagePath,
        packageTempPath,
        [this, &lastPercent](qint64 sent, qint64 total) {
            if (m_cancelRequested || total <= 0) {
                return !m_cancelRequested;
            }
            const int percent = static_cast<int>((sent * 100) / total);
            if (percent != lastPercent && (percent % 5 == 0 || percent == 100)) {
                lastPercent = percent;
                emit info(QString("上传进度：%1% (%2/%3 字节)")
                    .arg(percent)
                    .arg(sent)
                    .arg(total));
                processUiEvents();
            }
            return !m_cancelRequested;
        },
        &uploadError);

    if (m_cancelRequested) {
        emit error("部署已取消");
        return false;
    }

    if (!uploaded) {
        emit error(QString("SSH 上传失败：%1").arg(uploadError));
        return false;
    }

    const QString unpackCmd = QString("rm -rf %1 && mkdir -p %1 && "
                                      "unzip -o %2 -d %1 && "
                                      "chmod -R 777 %1 && "
                                      "rm -f %2 && sync")
                                  .arg(deployDir, packageTempPath);
    if (!runRemoteCommand(QString("解压 JIAOBEN.zip 到 %1 ...").arg(deployDir), unpackCmd, 180000, true)) {
        return false;
    }

    emit success(QString("脚本包部署完成：%1").arg(deployDir));
    return true;
}

bool FileDeployer::runRemoteCommand(const QString &message,
                                    const QString &cmd,
                                    int timeoutMs,
                                    bool echoOutput)
{
    if (m_cancelRequested) {
        emit error("部署已取消");
        return false;
    }

    emit info(message);
    processUiEvents();

    QString output;
    QString errorOutput;
    int exitCode = -1;
    const bool ok = m_commManager->executeCommand(cmd, output, &errorOutput, &exitCode, timeoutMs);
    if (echoOutput && !output.trimmed().isEmpty()) {
        emit raw(output.endsWith('\n') ? output : output + "\n");
    }
    if (!ok || exitCode != 0) {
        const QString details = !errorOutput.trimmed().isEmpty()
            ? errorOutput.trimmed()
            : output.trimmed();
        emit error(details.isEmpty()
            ? QString("远程命令执行失败，退出码：%1").arg(exitCode)
            : QString("远程命令执行失败：%1").arg(details));
        return false;
    }

    return true;
}

bool FileDeployer::ensurePackageReadable(const QString &packagePath)
{
    const QFileInfo info(packagePath);
    if (!info.exists() || !info.isFile()) {
        emit error(QString("未找到脚本包：%1").arg(packagePath));
        return false;
    }
    if (info.size() <= 0) {
        emit error(QString("脚本包为空：%1").arg(packagePath));
        return false;
    }
    if (!info.isReadable()) {
        emit error(QString("脚本包不可读：%1").arg(packagePath));
        return false;
    }
    return true;
}

void FileDeployer::processUiEvents() const
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}