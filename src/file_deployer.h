#ifndef FILE_DEPLOYER_H
#define FILE_DEPLOYER_H

#include "communication_manager.h"

#include <QObject>
#include <QString>

class FileDeployer : public QObject {
    Q_OBJECT

public:
    explicit FileDeployer(CommunicationManager *communicationManager,
                          QObject *parent = nullptr);

    bool deployPackage(const QString &packagePath);
    void cancel();
    bool isCancelled() const { return m_cancelRequested; }

signals:
    void info(const QString &message);
    void success(const QString &message);
    void error(const QString &message);
    void raw(const QString &text);

private:
    bool deployOverSsh(const QString &packagePath);
    bool runRemoteCommand(const QString &message,
                          const QString &cmd,
                          int timeoutMs = 120000,
                          bool echoOutput = true);
    bool ensurePackageReadable(const QString &packagePath);
    void processUiEvents() const;

    CommunicationManager *m_commManager{nullptr};
    bool m_cancelRequested{false};
};

#endif // FILE_DEPLOYER_H