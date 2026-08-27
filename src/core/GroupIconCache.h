#pragma once

#include <QHash>
#include <QPixmap>
#include <QString>

// ============================================================================
// Cache dos ícones de cargo enviados pelos administradores (icon_set /
// icon_get / icon_data do protocolo), com escopo POR SERVIDOR.
//
// Histórico do bug: o ícone era gravado em "cache/icons/<nome>" — caminho
// RELATIVO ao diretório de trabalho. Instalado em Program Files, o atalho do
// menu Iniciar deixa o cwd no próprio diretório de instalação, que não é
// gravável por usuários comuns: a gravação falhava em silêncio, o arquivo
// nunca existia para o delegado ler e o ícone de imagem nunca aparecia ao
// lado do nome (emoji/letra sempre apareceram porque renderizam direto da
// string do cargo, sem ir ao disco).
//
// Duas camadas agora:
//   1. memória — atualização instantânea mesmo se o disco falhar;
//   2. disco em QStandardPaths::CacheLocation (o mesmo padrão do
//      BadgeRegistry), gravável em qualquer instalação e persistente entre
//      execuções.
//
// Escopo por servidor (endereço conectado): dois servidores podem usar o
// MESMO nome de arquivo ("logo.png") com imagens diferentes — sem a chave
// por servidor, o ícone de um apareceria nos usuários do outro.
// ============================================================================
class GroupIconCache {
public:
    static GroupIconCache& instance();

    // Chave estável e segura para sistema de arquivos para um endereço de
    // servidor ("host:porta" — IPv6 tem ':' e não pode virar nome de dir no
    // Windows). Memoizada internamente: paint roda o tempo todo.
    static QString serverKey(const QString& serverAddress);

    // Pixmap pronto para a lista (16x14). Null quando não conhecemos o
    // ícone — o chamador então dispara iconRequested para buscá-lo.
    QPixmap pixmap(const QString& serverKey, const QString& name);

    // Guarda os bytes recebidos do servidor: decodifica, escala, guarda em
    // memória e grava no disco (best effort — falha de disco não impede a
    // exibição nesta sessão).
    void store(const QString& serverKey, const QString& name, const QByteArray& bytes);

    // Nome sanitizado no MESMO padrão do servidor (sanitizeFileName do
    // ServerCore): caminho de arquivo jamais escapa do diretório de cache.
    static QString safeName(const QString& name);

    // Diretório de cache absoluto (criado se possível).
    static QString cacheDir();

private:
    GroupIconCache() = default;

    QString diskPath(const QString& serverKey, const QString& safe) const;

    QHash<QString, QPixmap> m_pixmaps;   // "serverKey|safeName" -> pixmap 16x14
};
