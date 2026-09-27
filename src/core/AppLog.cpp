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
        const QString path = dir + "/halla.log";
        // v1.1.37: rotação — o arquivo crescia PARA SEMPRE (o AppLog nunca
        // apagava nada) e o "Registro do cliente" (que carrega o histórico
        // ao abrir) congelava a interface por segundos em instalações com
        // semanas de uso. Acima de 2 MiB o log atual vira halla.log.old
        // (substituindo o anterior) e um novo começa do zero: disco limitado
        // a ~4 MiB e abertura instantânea. A primeira escrita da sessão
        // (banner do motor de voz ao conectar) já dispara a rotação.
        qint64 size = QFile(path).size();
        if (size > 2 * 1024 * 1024) {
            const QString old = dir + "/halla.log.old";
            QFile::remove(old);
            QFile::rename(path, old);
        }
        m_file = new QFile(path);
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
