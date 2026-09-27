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
    // %APPDATA%\Halla\Halla\halla.log no Windows).
    // v1.1.35: o arquivo fica ABERTO em append — antes cada linha abria e
    // fechava o handle e reexecutava mkpath (stat de diretório) por chamada.
    // O LogDialog lê com handle próprio; o arquivo permanece legível de
    // fora, e o flush por linha mantém o conteúdo à prova de crash.
    if (!m_file) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        m_file = new QFile(dir + "/halla.log");
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
