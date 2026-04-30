#ifndef LOGGER_H
#define LOGGER_H

#include <QDate>
#include <QFile>
#include <QObject>
#include <QTextEdit>
#include <QScrollBar>
#include <QString>
#include <memory>

/**
 * @class Logger
 * @brief 日志管理器 - 负责所有日志的输出和格式化
 *
 * 功能特性:
 * - 统一的日志输出接口
 * - 智能的换行处理（保留原始换行符）
 * - 日志级别管理
 * - 彩色编码显示
 * - 自动滚动到最新行
 *
 * 日志级别:
 * - INFO: 普通信息（黑色）
 * - SUCCESS: 成功信息（绿色）
 * - ERROR: 错误信息（红色）
 * - RUNNING: 运行中提示（蓝色）
 * - RAW: 原始输出（不添加时间戳和级别标记）
 */
class Logger : public QObject {
    Q_OBJECT

public:
    /**
     * 构造函数
     * @param textEdit: 输出目标文本框
     * @param parent: 父对象
     */
    explicit Logger(QTextEdit *textEdit, QObject *parent = nullptr);

    /**
     * 析构函数
     */
    ~Logger();

    /**
     * 输出普通信息
     * @param message: 消息内容
     *
     * @example:
     * @code
     *   logger->info("脚本启动成功");
     * @endcode
     */
    void info(const QString &message);

    /**
     * 输出成功信息
     * @param message: 消息内容
     */
    void success(const QString &message);

    /**
     * 输出错误信息
     * @param message: 消息内容
     */
    void error(const QString &message);

    /**
     * 输出运行中提示
     * @param message: 消息内容
     */
    void running(const QString &message);

    /**
     * 输出原始文本（不添加时间戳和级别标记）
     * @param text: 原始文本，保留所有换行符和格式
     *
     * @note: 用于脚本直接输出的内容，原样显示
     *
     * @example:
     * @code
     *   logger->raw("Motor 1: calibrated\nMotor 2: calibrating...");
     * @endcode
     */
    void raw(const QString &text);

    /**
     * 清空所有日志
     */
    void clear();

    /**
     * 获取当前文本框内容
     * @return: 文本框中的所有文本
     */
    QString getText() const;

    /**
     * 设置是否启用时间戳
     * @param enabled: true=显示时间戳，false=隐藏
     */
    void setTimestampEnabled(bool enabled);

    /**
     * 设置最大日志行数（超出时自动删除最早的行）
     * @param maxLines: 最大行数，0表示不限制
     */
    void setMaxLines(int maxLines);

private:
    /**
     * 内部日志输出函数
     * @param level: 日志级别
     * @param message: 消息内容
     * @param isRaw: 是否为原始输出（不添加格式）
     */
    void logInternal(const QString &level, const QString &message, bool isRaw = false);

    /**
     * 获取颜色代码
     * @param level: 日志级别
     * @return: HTML颜色代码，如 "#ff0000"
     */
    QString getLevelColor(const QString &level) const;

    /**
     * 获取当前时间戳
     * @return: 格式化的时间戳，如 "2026-04-10 14:38:45"
     */
    QString getTimestamp() const;

    /**
     * 检查并删除超出的日志行
     */
    void trimExcessiveLines();
    bool ensureLogFileOpen();
    void writeToFile(const QString &text);
    void cleanupOldLogFiles();

private slots:
    /**
     * 响应滚动条值变化（用户手动滚动）：
     * 若滚动条在底部则启用自动滚动，否则禁用，避免打断用户查看历史日志
     */
    void onScrollBarValueChanged(int value);

    /**
     * 响应滚动条范围变化（内容追加导致文档变高）：
     * 若处于自动滚动模式，则随新内容自动滚到底部
     */
    void onScrollBarRangeChanged(int min, int max);

private:
    QTextEdit *m_textEdit;          ///< 目标文本框指针
    bool m_timestampEnabled;        ///< 是否显示时间戳
    int m_maxLines;                 ///< 最大日志行数（0=不限制）
    int m_currentLines;             ///< 当前日志行数
    bool m_autoScroll;              ///< 是否处于自动滚动模式（底部时为 true）
    QString m_logDirPath;           ///< 日志目录
    QDate m_currentLogDate;         ///< 当前打开日志文件的日期
    QFile m_logFile;                ///< 当前日志文件
};

#endif // LOGGER_H
