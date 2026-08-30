#pragma once

#include <QHash>
#include <QPixmap>
#include <QString>
#include <QStringList>

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

    // Pixmap pronto para a lista (24x21, KeepAspectRatio — o chamador usa
    // as dimensões reais para não esticar). Null quando não conhecemos o
    // ícone — o chamador então dispara iconRequested para buscá-lo.
    QPixmap pixmap(const QString& serverKey, const QString& name);

    // Guarda os bytes recebidos do servidor: decodifica, escala, guarda em
    // memória e grava no disco (best effort — falha de disco não impede a
    // exibição nesta sessão).
    void store(const QString& serverKey, const QString& name, const QByteArray& bytes);

    // Nome sanitizado no MESMO padrão do servidor (sanitizeFileName do
    // ServerCore): caminho de arquivo jamais escapa do diretório de cache.
    static QString safeName(const QString& name);

    // Nome de ícone que referencia uma IMAGEM enviada ao servidor
    // (icon_set). Compartilhado entre o delegado da árvore e o painel de
    // informações — os dois renderizam ícones de cargo e precisam da MESMA
    // regra do que é imagem (emoji/letra/sigla renderizam como texto).
    static bool isImageName(const QString& name);

    // Divide uma linha de cargo "<icone> <nome>" enviada pelo servidor
    // (applyGroup concatena sem separador explícito). Varre os espaços da
    // esquerda para a direita: a PRIMEIRA quebra cujo lado esquerdo é nome
    // de imagem delimita o ícone — cobre nomes de arquivo COM espaço
    // ("meu cargo.png ROTA"). Sem ícone de imagem: iconName fica vazio e o
    // label também (a linha inteira é o cargo). Compartilhado entre painel
    // de informações e tooltip da árvore — uma única regra de parse.
    static void splitRoleLine(const QString& roleLine, QString* iconName, QString* label);

    // Converte o campo "group" do servidor (linhas "<icone> <nome>"
    // separadas por \n) em uma lista de NOMES de cargos, sem nomes de
    // arquivo de ícone (ex.: "rota.png ROTA" -> "ROTA"). Usado onde só
    // interessa o texto limpo: combo de atribuição, banner de permissões.
    static QStringList cleanRoleNames(const QString& serverGroups);

    // Caminho absoluto do ícone no disco quando ele já está em cache
    // (para renderizar <img src="file:///..."> em rich text de tooltip).
    // Vazio quando não há arquivo — o chamador mostra só o nome do cargo.
    static QString iconFilePath(const QString& serverKey, const QString& name);

    // Porta de requisição COMPARTILHADA (árvore + painel de informações):
    // sem o ícone em mãos, re-tenta a cada 5 s (o upload pode estar a
    // caminho); com o ícone, um único re-fetch por execução — troca cópia
    // antiga de disco pela versão atual do servidor sem spamear pedidos.
    // Compartilhar o estado entre os dois consumidores evita pedido
    // duplicado quando a árvore e o painel pedem o mesmo ícone juntos.
    static bool shouldRequest(const QString& requestKey, bool haveIt);

    // Diretório de cache absoluto (criado se possível).
    static QString cacheDir();

private:
    GroupIconCache() = default;

    QString diskPath(const QString& serverKey, const QString& safe) const;

    QHash<QString, QPixmap> m_pixmaps;   // "serverKey|safeName" -> pixmap 24x21
};
