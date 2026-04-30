#include "test_result_manager.h"

#include <QFile>
#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QRegularExpression>
#include <QTextStream>

TestResultManager::TestResultManager(QGroupBox *container, QObject *parent)
    : QObject(parent)
    , m_container(container)
{
    if (m_container) {
        m_layout = new QGridLayout(m_container);
        m_layout->setContentsMargins(12, 16, 12, 12);
        m_layout->setHorizontalSpacing(10);
        m_layout->setVerticalSpacing(10);
        m_container->installEventFilter(this);
        showPlaceholder("未加载测试映射表");
    }
}

bool TestResultManager::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_container && event->type() == QEvent::Resize) {
        relayoutCards();
    }
    return QObject::eventFilter(watched, event);
}

bool TestResultManager::loadFromCsv(const QString &csvPath, QString *errorMessage)
{
    QFile file(csvPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QString("无法打开映射表：%1").arg(csvPath);
        }
        showPlaceholder("未找到映射表 CSV");
        return false;
    }

    QVector<MappingEntry> entries;
    QTextStream stream(&file);
    stream.setCodec("UTF-8");

    int nameColumn = 0;
    int scriptColumn = 1;
    int successColumn = 2;
    int failureColumn = 3;
    bool firstDataLine = true;

    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (line.trimmed().isEmpty()) {
            continue;
        }

        const QStringList columns = parseCsvLine(line);
        if (columns.size() < 4) {
            continue;
        }

        if (firstDataLine) {
            firstDataLine = false;
            const bool hasHeader = columnIndex(columns, {"测试项", "项目名称", "项目", "name"}, -1) >= 0
                || columnIndex(columns, {"测试脚本", "脚本", "script", "path"}, -1) >= 0
                || columnIndex(columns, {"成功码", "success", "suc"}, -1) >= 0
                || columnIndex(columns, {"错误码", "失败码", "error", "err"}, -1) >= 0;
            if (hasHeader) {
                nameColumn = columnIndex(columns, {"测试项", "项目名称", "项目", "name"}, 0);
                scriptColumn = columnIndex(columns, {"测试脚本", "脚本", "script", "path"}, 1);
                successColumn = columnIndex(columns, {"成功码", "success", "suc"}, 2);
                failureColumn = columnIndex(columns, {"错误码", "失败码", "error", "err"}, 3);
                continue;
            }
        }

        const int requiredColumn = qMax(qMax(nameColumn, scriptColumn), qMax(successColumn, failureColumn));
        if (columns.size() <= requiredColumn) {
            continue;
        }

        MappingEntry entry;
        entry.name = columns[nameColumn].trimmed();
        entry.script = columns[scriptColumn].trimmed();
        entry.successCode = columns[successColumn].trimmed().toUpper();
        entry.failureCode = columns[failureColumn].trimmed().toUpper();
        if (!entry.name.isEmpty()) {
            entries.push_back(entry);
        }
    }

    if (entries.isEmpty()) {
        if (errorMessage) {
            *errorMessage = "映射表为空或格式不正确，CSV 至少需要：测试项,测试脚本,成功码,错误码";
        }
        showPlaceholder("映射表为空或格式不正确");
        return false;
    }

    rebuildCards(entries);
    return true;
}

void TestResultManager::resetAll()
{
    for (TestResultCard *card : m_cards) {
        card->setStatus(TestResultCard::Status::Pending);
    }
}

void TestResultManager::applyOutputText(const QString &text)
{
    static const QRegularExpression codeRegex("\\b(?:SUC|ERR)\\d+\\b",
                                              QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator iterator = codeRegex.globalMatch(text);
    while (iterator.hasNext()) {
        const QString code = iterator.next().captured(0).toUpper();
        if (m_successCodes.contains(code)) {
            m_successCodes.value(code)->setStatus(TestResultCard::Status::Success);
        } else if (m_failureCodes.contains(code)) {
            m_failureCodes.value(code)->setStatus(TestResultCard::Status::Failure);
        } else {
            emit unknownCodeReceived(code);
        }
    }
}

QStringList TestResultManager::parseCsvLine(const QString &line) const
{
    QStringList columns;
    QString current;
    bool inQuotes = false;

    for (int index = 0; index < line.size(); ++index) {
        const QChar ch = line[index];
        if (ch == '"') {
            if (inQuotes && index + 1 < line.size() && line[index + 1] == '"') {
                current.append('"');
                ++index;
            } else {
                inQuotes = !inQuotes;
            }
        } else if (ch == ',' && !inQuotes) {
            columns << current;
            current.clear();
        } else {
            current.append(ch);
        }
    }
    columns << current;
    return columns;
}

int TestResultManager::columnIndex(const QStringList &headers, const QStringList &names, int fallback) const
{
    for (const QString &name : names) {
        for (int index = 0; index < headers.size(); ++index) {
            if (headers[index].trimmed().compare(name, Qt::CaseInsensitive) == 0) {
                return index;
            }
        }
    }
    return fallback;
}

void TestResultManager::clearCards()
{
    if (!m_layout) {
        return;
    }

    while (QLayoutItem *item = m_layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    m_entries.clear();
    m_cards.clear();
    m_successCodes.clear();
    m_failureCodes.clear();
    m_placeholder = nullptr;
    m_currentColumns = 0;
    m_currentRows = 0;
}

void TestResultManager::rebuildCards(const QVector<MappingEntry> &entries)
{
    clearCards();
    m_entries = entries;

    for (int index = 0; index < entries.size(); ++index) {
        auto *card = new TestResultCard(entries[index].name, m_container);
        m_cards.push_back(card);

        if (!entries[index].successCode.isEmpty()) {
            m_successCodes.insert(entries[index].successCode, card);
        }
        if (!entries[index].failureCode.isEmpty()) {
            m_failureCodes.insert(entries[index].failureCode, card);
        }
    }
    relayoutCards();
}

void TestResultManager::relayoutCards()
{
    if (!m_layout || m_cards.isEmpty()) {
        return;
    }

    const int availableWidth = qMax(1, m_container ? m_container->width() : 1);
    const int columns = qBound(1, availableWidth / 220, 6);
    const int rows = (m_cards.size() + columns - 1) / columns;
    if (columns == m_currentColumns && rows == m_currentRows && m_layout->count() == m_cards.size()) {
        return;
    }

    const int columnLimit = qMax(columns, m_currentColumns);
    for (int column = 0; column < columnLimit; ++column) {
        m_layout->setColumnStretch(column, 0);
        m_layout->setColumnMinimumWidth(column, 0);
    }

    const int rowLimit = qMax(rows, m_currentRows);
    for (int row = 0; row < rowLimit; ++row) {
        m_layout->setRowStretch(row, 0);
        m_layout->setRowMinimumHeight(row, 0);
    }

    while (QLayoutItem *item = m_layout->takeAt(0)) {
        delete item;
    }

    for (int index = 0; index < m_cards.size(); ++index) {
        m_layout->addWidget(m_cards[index], index / columns, index % columns);
    }

    for (int column = 0; column < columns; ++column) {
        m_layout->setColumnStretch(column, 1);
    }
    for (int row = 0; row < rows; ++row) {
        m_layout->setRowStretch(row, 1);
    }

    m_currentColumns = columns;
    m_currentRows = rows;
}

void TestResultManager::showPlaceholder(const QString &message)
{
    clearCards();
    if (!m_layout) {
        return;
    }

    m_placeholder = new QLabel(message, m_container);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setStyleSheet("color: #66788a; font-size: 11pt; padding: 10px;");
    m_layout->addWidget(m_placeholder, 0, 0);
}
