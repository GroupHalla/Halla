#include "AppLog.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>

AppLog& AppLog::instance() {
    static AppLog log;
    return log;
}

QString AppLog::levelName(Level lvl) {
    switch (lvl) {
        case Info:    return "INFO";
        case Warning: return "AVISO";
        case Error:   return "ERRO";
        case Debug:   return "DEPURAÇÃO";
    }
    return "INFO";
}

void AppLog::write(Level lvl, const QString& msg) {
    const QString ts = QDateTime::currentDateTime().toString("dd/MM/yyyy HH:mm:ss");

    if (!m_file) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        const QString path = dir + "/halla.log";
        // Rotate the log file once it exceeds 2 MiB to bound disk usage and keep startup fast.
        const qint64 size = QFile(path).size();
        if (size > 2 * 1024 * 1024) {
            const QString old = dir + "/halla.log.old";
            QFile::remove(old);
            QFile::rename(path, old);
        }
        m_file = new QFile(path, this);
        m_file->open(QIODevice::Append | QIODevice::Text);
    }
    if (m_file->isOpen()) {
        m_file->write(QStringLiteral("[%1] [%2] %3\n")
                          .arg(ts, levelName(lvl), msg)
                          .toUtf8());
        m_file->flush();
    }

    emit message(static_cast<int>(lvl), ts, msg);
}

void AppLog::clearLogFile() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (m_file) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }
    QFile::remove(dir + "/halla.log");
    QFile::remove(dir + "/halla.log.old");
    emit cleared();
}
