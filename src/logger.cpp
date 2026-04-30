#include "logger.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextBlock>
#include <QScrollBar>

Logger::Logger(QTextEdit *textEdit, QObject *parent)
    : QObject(parent)
    , m_textEdit(textEdit)
    , m_timestampEnabled(true)
    , m_maxLines(50)
    , m_currentLines(0)
    , m_autoScroll(true)
{
    if (m_textEdit) {
        m_textEdit->setReadOnly(true);

        QScrollBar *vbar = m_textEdit->verticalScrollBar();
        // 用户手动滚动时：判断是否离开底部，相应地开启/关闭自动滚动
        connect(vbar, &QScrollBar::valueChanged,
                this, &Logger::onScrollBarValueChanged);
        // 内容增加导致文档变高时：若处于自动滚动模式，追到新底部
        connect(vbar, &QScrollBar::rangeChanged,
                this, &Logger::onScrollBarRangeChanged);
    }

    m_logDirPath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("logs");
    ensureLogFileOpen();
}

Logger::~Logger()
{
    if (m_logFile.isOpen()) {
        m_logFile.close();
    }
}

void Logger::info(const QString &message)
{
    logInternal("INFO", message, false);
}

void Logger::success(const QString &message)
{
    logInternal("SUCCESS", message, false);
}

void Logger::error(const QString &message)
{
    logInternal("ERROR", message, false);
}

void Logger::running(const QString &message)
{
    logInternal("RUNNING", message, false);
}

void Logger::raw(const QString &text)
{
    // 原始输出直接显示，不添加格式
    if (!m_textEdit || text.isEmpty()) {
        return;
    }

    // 使用文档 cursor 插入，不触发 ensureCursorVisible，保留用户视口位置
    QTextCursor cursor(m_textEdit->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    writeToFile(text);
    // 滚动由 onScrollBarRangeChanged 根据 m_autoScroll 自动处理

    // 统计新增行数
    int newLines = text.count('\n');
    m_currentLines += newLines;

    // 检查是否需要删除超出的行
    trimExcessiveLines();
}

void Logger::clear()
{
    if (m_textEdit) {
        m_textEdit->clear();
        m_currentLines = 0;
        m_autoScroll = true;  // 清空后恢复自动滚动
    }
}

QString Logger::getText() const
{
    if (m_textEdit) {
        return m_textEdit->toPlainText();
    }
    return QString();
}

void Logger::setTimestampEnabled(bool enabled)
{
    m_timestampEnabled = enabled;
}

void Logger::setMaxLines(int maxLines)
{
    m_maxLines = maxLines;
}

void Logger::logInternal(const QString &level, const QString &message, bool isRaw)
{
    if (!m_textEdit) {
        return;
    }

    QString output;
    QString fileOutput;

    if (isRaw) {
        // 原始输出直接显示
        output = message;
        fileOutput = message;
    } else {
        // 格式化输出
        if (m_timestampEnabled) {
            QString timestamp = getTimestamp();
            fileOutput = QString("[%1] %2: %3").arg(timestamp, level, message);
        } else {
            fileOutput = QString("[%1] %2").arg(level, message);
        }

        // 添加颜色编码（使用HTML）
        QString color = getLevelColor(level);
        output = QString("<font color='%1'>%2</font>").arg(color, fileOutput.toHtmlEscaped());
    }

    // 使用文档 cursor 插入，不触发 ensureCursorVisible，保留用户视口位置
    QTextCursor cursor(m_textEdit->document());
    cursor.movePosition(QTextCursor::End);

    // 输出日志
    if (isRaw) {
        cursor.insertText(output);
    } else {
        cursor.insertHtml(output);
        cursor.insertText("\n");  // 手动添加换行
        fileOutput += "\n";
    }
    writeToFile(fileOutput);

    // 滚动由 onScrollBarRangeChanged 根据 m_autoScroll 自动处理

    // 统计行数
    if (!isRaw) {
        m_currentLines++;
    } else {
        m_currentLines += output.count('\n') + 1;
    }

    // 检查是否需要删除超出的行
    trimExcessiveLines();
}

QString Logger::getLevelColor(const QString &level) const
{
    if (level == "INFO") {
        return "#000000";  // 黑
    } else if (level == "SUCCESS") {
        return "#008000";  // 绿
    } else if (level == "ERROR") {
        return "#ff0000";  // 红
    } else if (level == "RUNNING") {
        return "#0000ff";  // 蓝
    } else {
        return "#808080";  // 灰
    }
}

QString Logger::getTimestamp() const
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
}

void Logger::onScrollBarValueChanged(int value)
{
    if (!m_textEdit) return;
    QScrollBar *bar = m_textEdit->verticalScrollBar();
    // 滚动条贴底 → 用户回到底部，启用自动滚动
    // 滚动条离开底部 → 用户正在查看历史，停止自动滚动
    m_autoScroll = (value == bar->maximum());
}

void Logger::onScrollBarRangeChanged(int /*min*/, int max)
{
    if (m_autoScroll && m_textEdit) {
        // 内容增加时，自动追到新的底部
        m_textEdit->verticalScrollBar()->setValue(max);
    }
}

void Logger::trimExcessiveLines()
{
    if (m_maxLines <= 0 || m_currentLines <= m_maxLines) {
        return;
    }

    // 删除超出的行
    QTextDocument *doc = m_textEdit->document();
    int linesToRemove = m_currentLines - m_maxLines;

    QTextBlock block = doc->findBlockByNumber(linesToRemove);
    QTextCursor cursor(doc);
    cursor.setPosition(0);
    cursor.setPosition(block.position(), QTextCursor::KeepAnchor);
    cursor.removeSelectedText();

    m_currentLines = m_maxLines;
}

bool Logger::ensureLogFileOpen()
{
    const QDate today = QDate::currentDate();
    if (m_logFile.isOpen() && m_currentLogDate == today) {
        return true;
    }

    if (m_logFile.isOpen()) {
        m_logFile.close();
    }

    QDir dir(m_logDirPath);
    if (!dir.exists() && !dir.mkpath(".")) {
        qWarning() << "Failed to create log directory:" << m_logDirPath;
        return false;
    }

    m_currentLogDate = today;
    const QString fileName = QString("Robot_ToolChain_%1.log").arg(today.toString("yyyy-MM-dd"));
    m_logFile.setFileName(dir.absoluteFilePath(fileName));
    if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        qWarning() << "Failed to open log file:" << m_logFile.fileName() << m_logFile.errorString();
        return false;
    }

    cleanupOldLogFiles();
    return true;
}

void Logger::writeToFile(const QString &text)
{
    if (text.isEmpty() || !ensureLogFileOpen()) {
        return;
    }

    QTextStream stream(&m_logFile);
    stream.setCodec("UTF-8");
    stream << text;
    stream.flush();
    m_logFile.flush();
}

void Logger::cleanupOldLogFiles()
{
    QDir dir(m_logDirPath);
    const QFileInfoList files = dir.entryInfoList({"Robot_ToolChain_*.log"}, QDir::Files, QDir::Name);
    const QDate oldestKeptDate = QDate::currentDate().addDays(-6);

    for (const QFileInfo &fileInfo : files) {
        const QString baseName = fileInfo.completeBaseName();
        const QString dateText = baseName.mid(QString("Robot_ToolChain_").size());
        const QDate fileDate = QDate::fromString(dateText, "yyyy-MM-dd");
        if (fileDate.isValid() && fileDate < oldestKeptDate) {
            QFile::remove(fileInfo.absoluteFilePath());
        }
    }
}
