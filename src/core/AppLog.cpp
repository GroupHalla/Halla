#include "AppLog.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QTextStream>

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

    // Log em arquivo (~/.local/share/Halla/Halla/halla.log no Linux,
    // %APPDATA%\Halla\Halla\halla.log no Windows)
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    QFile f(dir + "/halla.log");
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&f);
        out << "[" << ts << "] [" << levelName(lvl) << "] " << msg << "\n";
    }

    emit message(static_cast<int>(lvl), ts, msg);
}
