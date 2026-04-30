#ifndef TEST_RESULT_MANAGER_H
#define TEST_RESULT_MANAGER_H

#include "test_result_card.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

class QGridLayout;
class QGroupBox;
class QLabel;
class QEvent;

class TestResultManager : public QObject {
    Q_OBJECT

public:
    struct MappingEntry {
        QString name;
        QString script;
        QString successCode;
        QString failureCode;
    };

    explicit TestResultManager(QGroupBox *container, QObject *parent = nullptr);

    bool loadFromCsv(const QString &csvPath, QString *errorMessage = nullptr);
    void resetAll();
    void applyOutputText(const QString &text);
    int cardCount() const { return m_cards.size(); }
    const QVector<MappingEntry> &entries() const { return m_entries; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void unknownCodeReceived(const QString &code);

private:
    QStringList parseCsvLine(const QString &line) const;
    int columnIndex(const QStringList &headers, const QStringList &names, int fallback) const;
    void clearCards();
    void rebuildCards(const QVector<MappingEntry> &entries);
    void relayoutCards();
    void showPlaceholder(const QString &message);

    QGroupBox *m_container{nullptr};
    QGridLayout *m_layout{nullptr};
    QLabel *m_placeholder{nullptr};
    QVector<MappingEntry> m_entries;
    QVector<TestResultCard *> m_cards;
    QHash<QString, TestResultCard *> m_successCodes;
    QHash<QString, TestResultCard *> m_failureCodes;
    int m_currentColumns{0};
    int m_currentRows{0};
};

#endif // TEST_RESULT_MANAGER_H