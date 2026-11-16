#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>

class QFile;

// Centralized client event logger with persistent log-file rotation.
class AppLog : public QObject {
    Q_OBJECT
public:
    enum Level { Info = 0, Warning = 1, Error = 2, Debug = 3 };

    static AppLog& instance();

    void write(Level lvl, const QString& msg);
    void clearLogFile();

    static void info(const QString& msg)    { instance().write(Info, msg); }
    static void warn(const QString& msg)    { instance().write(Warning, msg); }
    static void error(const QString& msg)   { instance().write(Error, msg); }
    static void debug(const QString& msg)   { instance().write(Debug, msg); }

    static QString levelName(Level lvl);

signals:
    void message(int level, const QString& timestamp, const QString& text);
    void cleared();

private:
    AppLog() = default;
    QFile* m_file = nullptr;
};
