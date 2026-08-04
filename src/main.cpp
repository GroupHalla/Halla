#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QTimer>
#include <QPixmap>
#include <QDir>
#include <QTranslator>
#include <QLocale>
#include <QMap>
#include "app/MainWindow.h"
#include "gui/Icons.h"
#include "core/Settings.h"
#include "dialogs/ConnectDialog.h"
#include "dialogs/OptionsDialog.h"
#include "dialogs/ChannelDialog.h"
#include "version.h"

// Modo especial para capturas de tela automatizadas (usado em desenvolvimento):
//   Halla --shot <caminho.png> <janela>   (janela: main | demo | connect | options | channel)
static int takeShot(QApplication& app, const QString& path, const QString& what) {
    QTimer timer;
    timer.setSingleShot(true);

    MainWindow w;

    auto grab = [&]() {
        if (what == "demo") w.loadDemoState();
        w.resize(1706, 922);
        w.show();
        app.processEvents();
        QTimer::singleShot(120, &w, [&w, path, &app] {
            w.grab().save(path);
            app.quit();
        });
    };

    if (what == "connect") {
        ConnectDialog* dlg = new ConnectDialog(&w);
        dlg->setNickname(QStringLiteral("HallaUser"));
        dlg->setAddress(QStringLiteral("meuservidor.exemplo.com"));
        w.resize(900, 600);
        w.show();
        dlg->show();
        app.processEvents();
        QTimer::singleShot(200, &w, [dlg, path, &app] {
            dlg->grab().save(path);
            app.quit();
        });
    } else if (what.startsWith(QStringLiteral("options"))) {
        OptionsDialog* dlg = new OptionsDialog(&w);
        const int colon = what.indexOf(QLatin1Char(':'));
        if (colon >= 0) dlg->selectPage(what.mid(colon + 1));
        w.resize(900, 600);
        w.show();
        dlg->show();
        app.processEvents();
        QTimer::singleShot(200, &w, [dlg, path, &app] {
            dlg->grab().save(path);
            app.quit();
        });
    } else if (what == "channel") {
        w.loadDemoState();
        ServerData* d = nullptr;
        // usa o diálogo de canal com dados da aba demo
        ChannelDialog* dlg = new ChannelDialog(QObject::tr("Criar canal"), d, nullptr, &w);
        w.show();
        dlg->show();
        app.processEvents();
        QTimer::singleShot(200, &w, [dlg, path, &app] {
            dlg->grab().save(path);
            app.quit();
        });
    } else {
        grab();
    }

    return app.exec();
}

class HallaTranslator : public QTranslator {
    Q_OBJECT
public:
    explicit HallaTranslator(QObject* parent = nullptr) : QTranslator(parent) {
        m_lang = QLocale::system().language();
        setupTranslations();
    }

    bool isEmpty() const override { return false; }

    QString translate(const char* context, const char* sourceText,
                      const char* disambiguation = nullptr, int n = -1) const override {
        Q_UNUSED(context);
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        
        QString key = QString::fromUtf8(sourceText);
        
        if (m_lang == QLocale::English) {
            if (m_en.contains(key)) return m_en.value(key);
        } else if (m_lang == QLocale::Spanish) {
            if (m_es.contains(key)) return m_es.value(key);
        }
        return key;
    }

private:
    QLocale::Language m_lang;
    QMap<QString, QString> m_en;
    QMap<QString, QString> m_es;

    void setupTranslations() {
        m_en["&Conexões"] = "&Connections";
        m_en["Conectar..."] = "Connect...";
        m_en["Conectar em nova aba..."] = "Connect in new tab...";
        m_en["Desconectar"] = "Disconnect";
        m_en["Desconectar de todos os servidores"] = "Disconnect from all servers";
        m_en["Conexões recentes"] = "Recent connections";
        m_en["Sair"] = "Exit";
        m_en["&Marcadores"] = "&Bookmarks";
        m_en["Adicionar aos marcadores..."] = "Add to bookmarks...";
        m_en["Gerenciar marcadores..."] = "Manage bookmarks...";
        m_en["Ausente"] = "Away";
        m_en["Mudo (microfone)"] = "Mute (Microphone)";
        m_en["Mudo (alto-falantes)"] = "Mute (Speakers)";
        m_en["Alterar apelido..."] = "Change nickname...";
        m_en["Alternar comandante do canal"] = "Toggle channel commander";
        m_en["Iniciar gravação"] = "Start recording";
        m_en["Parar gravação"] = "Stop recording";
        m_en["Opções..."] = "Options...";
        m_en["A&juda"] = "H&elp";
        m_en["Sobre o Halla"] = "About Halla";
        m_en["Verificar atualizações"] = "Check for updates";
        m_en["Informações de conexão..."] = "Connection Info...";
        m_en["Informações de Conexão do Servidor"] = "Server Connection Info";
        m_en["Informações de Conexão"] = "Connection Info";
        m_en["Nome do Servidor:"] = "Server Name:";
        m_en["Endereço:"] = "Address:";
        m_en["Versão do Servidor:"] = "Server Version:";
        m_en["Plataforma:"] = "Platform:";
        m_en["Clientes Conectados:"] = "Connected Clients:";
        m_en["Tempo de Conexão (Uptime):"] = "Connection Uptime:";
        m_en["Ping atual:"] = "Current Ping:";
        m_en["Você não está conectado a nenhum servidor."] = "You are not connected to any server.";
        m_en["Nova lista de sussurro"] = "New whisper list";
        m_en["Lista de sussurros"] = "Whisper list";
        m_en["Listas de sussurros sincronizadas"] = "Synchronized whisper lists";
        m_en["Listas de sussurros locais"] = "Local whisper lists";
        m_en["Nenhum item neste painel..."] = "No items in this panel...";
        m_en["Novo"] = "New";
        m_en["Remover"] = "Remove";
        m_en["Renomear"] = "Rename";
        m_en["Recarregar"] = "Reload";
        m_en["Tecla de atalho:"] = "Hotkey:";
        m_en["Tecla de atalho para resposta:"] = "Reply hotkey:";
        m_en["Enviar sussurro para:"] = "Send whisper to:";
        m_en["Clientes & canais"] = "Clients & channels";
        m_en["Grupos de servidores"] = "Server groups";
        m_en["Grupos de canais"] = "Channel groups";
        m_en["Árvore de Alvos (Caixa Branca Principal)"] = "Target Tree (Main White Box)";
        m_en["Árvore do Servidor"] = "Server Tree";
        m_en["Filtro:"] = "Filter:";
        m_en["Ver tudo"] = "View all";
        m_en["Canais"] = "Channels";
        m_en["Clientes"] = "Clients";
        m_en["Pesquisar..."] = "Search...";
        m_en["Usar chave de privilégio..."] = "Use privilege key...";
        m_en["Usar chave de privilégio"] = "Use privilege key";
        m_en["Chave de privilégio"] = "Privilege key";
        m_en["Digite a chave recebida do administrador"] = "Enter the key received from the administrator";
        m_en["Chave:"] = "Key:";
        m_en["Mover para o seu canal"] = "Move to your channel";
        m_en["Silenciar"] = "Mute";
        m_en["Definir volume..."] = "Set volume...";
        m_en["Registrar reclamação..."] = "Register complaint...";
        m_en["Cutucar"] = "Poke";
        m_en["Enviar uma cutucada para <b>%1</b>:"] = "Send a poke to <b>%1</b>:";
        m_en["Ei!"] = "Hey!";
        m_en["Expulsar do canal"] = "Kick from channel";
        m_en["Expulsar do servidor"] = "Kick from server";
        m_en["Banir cliente"] = "Ban client";
        m_en["Motivo (opcional)"] = "Reason (optional)";
        m_en["Duração do baneo:"] = "Ban duration:";
        m_en["Duração do banimento:"] = "Ban duration:";
        m_en["permanente"] = "permanent";
        m_en["Volume"] = "Volume";
        m_en["Volume de reprodução de <b>%1</b>:"] = "Playback volume of <b>%1</b>:";
        m_en["Usar o melhor modo automaticamente"] = "Use best mode automatically";
        m_en["Dispositivo de reprodução:"] = "Playback device:";
        m_en["Dispositivo de captura:"] = "Capture device:";
        m_en["Padrão"] = "Default";
        m_en["Ativação de voz"] = "Voice activation";
        m_en["Pressionar para falar (PTT)"] = "Push-to-talk (PTT)";
        m_en["Definir mais teclas de atalho"] = "Set more hotkeys";
        m_en["Capturar atalho"] = "Capture hotkey";
        m_en["Pressione uma tecla ou botão do mouse..."] = "Press a key or a mouse button...";
        m_en["Pressione uma tecla ou botão do mouse...  (Esc limpa)"] = "Press a key or a mouse button... (Esc clears)";
        m_en["Clique aqui e pressione uma tecla ou botão do mouse"] = "Click here and press a key or a mouse button";
        m_en["Definir descrição do cliente"] = "Set client description";
        m_en["Alterar apelido"] = "Change nickname";
        m_en["Ver avatar"] = "View avatar";
        m_en["Enviar mensagem"] = "Send message";
        m_en["Criar canal"] = "Create channel";
        m_en["Criar sub-canal"] = "Create sub-channel";
        m_en["Editar canal"] = "Edit channel";
        m_en["Excluir canal"] = "Delete channel";
        m_en["Ver descrição do canal"] = "View channel description";
        m_en["Alternar para o canal"] = "Switch to channel";
        m_en["Desconectado"] = "Disconnected";
        m_en["Conectado como %1"] = "Connected as %1";

        m_es["&Conexões"] = "&Conexiones";
        m_es["Conectar..."] = "Conectar...";
        m_es["Conectar em nova aba..."] = "Conectar en nueva pestaña...";
        m_es["Desconectar"] = "Desconectar";
        m_es["Desconectar de todos os servidores"] = "Desconectar de todos los servidores";
        m_es["Conexões recentes"] = "Conexiones recientes";
        m_es["Sair"] = "Salir";
        m_es["&Marcadores"] = "&Marcadores";
        m_es["Adicionar aos marcadores..."] = "Añadir a marcadores...";
        m_es["Gerenciar marcadores..."] = "Administrar marcadores...";
        m_es["Ausente"] = "Ausente";
        m_es["Mudo (microfone)"] = "Silenciar (Micrófono)";
        m_es["Mudo (alto-falantes)"] = "Silenciar (Altavoces)";
        m_es["Alterar apelido..."] = "Cambiar apodo...";
        m_es["Alternar comandante do canal"] = "Alternar comandante del canal";
        m_es["Iniciar gravação"] = "Iniciar grabación";
        m_es["Parar gravação"] = "Detener grabación";
        m_es["Opções..."] = "Opciones...";
        m_es["A&juda"] = "A&yuda";
        m_es["Sobre o Halla"] = "Acerca de Halla";
        m_es["Verificar atualizações"] = "Buscar actualizaciones";
        m_es["Informações de conexão..."] = "Info de conexión...";
        m_es["Informações de Conexão do Servidor"] = "Información de Conexión del Servidor";
        m_es["Informações de Conexão"] = "Información de Conexión";
        m_es["Nome do Servidor:"] = "Nombre del Servidor:";
        m_es["Endereço:"] = "Dirección:";
        m_es["Versão do Servidor:"] = "Versión del Servidor:";
        m_es["Plataforma:"] = "Plataforma:";
        m_es["Clientes Conectados:"] = "Clientes Conectados:";
        m_es["Tempo de Conexão (Uptime):"] = "Tiempo de Conexión (Uptime):";
        m_es["Ping atual:"] = "Ping actual:";
        m_es["Você não está conectado a nenhum servidor."] = "No estás conectado a ningún servidor.";
        m_es["Nova lista de sussurro"] = "Nueva lista de susurro";
        m_es["Lista de sussurros"] = "Lista de susurros";
        m_es["Listas de sussurros sincronizadas"] = "Listas de susurros sincronizadas";
        m_es["Listas de sussurros locais"] = "Listas de susurros locales";
        m_es["Nenhum item neste painel..."] = "Ningún elemento en este panel...";
        m_es["Novo"] = "Nuevo";
        m_es["Remover"] = "Eliminar";
        m_es["Renomear"] = "Renombrar";
        m_es["Recarregar"] = "Recargar";
        m_es["Tecla de atalho:"] = "Tecla de acceso rápido:";
        m_es["Tecla de atalho para resposta:"] = "Tecla de respuesta:";
        m_es["Enviar sussurro para:"] = "Enviar susurro a:";
        m_es["Clientes & canais"] = "Clientes y canales";
        m_es["Grupos de servidores"] = "Grupos de servidores";
        m_es["Grupos de canais"] = "Grupos de canales";
        m_es["Árvore de Alvos (Caixa Branca Principal)"] = "Árbol de Objetivos (Caja Blanca)";
        m_es["Árvore do Servidor"] = "Árbol del Servidor";
        m_es["Filtro:"] = "Filtro:";
        m_es["Ver tudo"] = "Ver todo";
        m_es["Canais"] = "Canales";
        m_es["Clientes"] = "Clientes";
        m_es["Pesquisar..."] = "Buscar...";
        m_es["Usar chave de privilégio..."] = "Usar clave de privilegio...";
        m_es["Usar chave de privilégio"] = "Usar clave de privilegio";
        m_es["Chave de privilégio"] = "Clave de privilegio";
        m_es["Digite a chave recebida do administrador"] = "Escriba la clave recibida del administrador";
        m_es["Chave:"] = "Clave:";
        m_es["Mover para o seu canal"] = "Mover a tu canal";
        m_es["Silenciar"] = "Silenciar";
        m_es["Definir volume..."] = "Establecer volumen...";
        m_es["Registrar reclamação..."] = "Registrar queja...";
        m_es["Cutucar"] = "Dar un toque";
        m_es["Enviar uma cutucada para <b>%1</b>:"] = "Enviar un toque a <b>%1</b>:";
        m_es["Ei!"] = "¡Oye!";
        m_es["Expulsar do canal"] = "Expulsar del canal";
        m_es["Expulsar do servidor"] = "Expulsar del servidor";
        m_es["Banir cliente"] = "Banear cliente";
        m_es["Motivo (opcional)"] = "Motivo (opcional)";
        m_es["Duração do banimento:"] = "Duración del baneo:";
        m_es["permanente"] = "permanente";
        m_es["Volume"] = "Volumen";
        m_es["Volume de reprodução de <b>%1</b>:"] = "Volumen de reproducción de <b>%1</b>:";
        m_es["Usar o melhor modo automaticamente"] = "Usar el mejor modo automáticamente";
        m_es["Dispositivo de reprodução:"] = "Dispositivo de reproducción:";
        m_es["Dispositivo de captura:"] = "Dispositivo de captura:";
        m_es["Padrão"] = "Predeterminado";
        m_es["Ativação de voz"] = "Activación de voz";
        m_es["Pressionar para falar (PTT)"] = "Presionar para hablar (PTT)";
        m_es["Definir mais teclas de atalho"] = "Definir más atajos";
        m_es["Capturar atalho"] = "Capturar atajo";
        m_es["Pressione uma tecla ou botão do mouse..."] = "Presione una tecla o botón del mouse...";
        m_es["Pressione uma tecla ou botão do mouse...  (Esc limpa)"] = "Presione una tecla o botón del mouse... (Esc limpia)";
        m_es["Clique aqui e pressione uma tecla ou botão do mouse"] = "Haga clic aquí y presione una tecla o botón del mouse";
        m_es["Definir descrição do cliente"] = "Definir descripción del cliente";
        m_es["Alterar apelido"] = "Cambiar apodo";
        m_es["Ver avatar"] = "Ver avatar";
        m_es["Enviar mensagem"] = "Enviar mensaje";
        m_es["Criar canal"] = "Crear canal";
        m_es["Criar sub-canal"] = "Crear subcanal";
        m_es["Editar canal"] = "Editar canal";
        m_es["Excluir canal"] = "Eliminar canal";
        m_es["Ver descrição do canal"] = "Ver descripción del canal";
        m_es["Alternar para o canal"] = "Cambiar al canal";
        m_es["Desconectado"] = "Desconectado";
        m_es["Conectado como %1"] = "Conectado como %1";
    }
};

int main(int argc, char* argv[]) {
    Q_INIT_RESOURCE(halla);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Halla"));
    QApplication::setOrganizationName(QStringLiteral("Halla"));
    QApplication::setApplicationDisplayName(QStringLiteral("Halla"));
    QApplication::setApplicationVersion(QString::fromUtf8(halla::kAppVersion));
    QApplication::setWindowIcon(QIcon(HIcons::appIcon(64)));

    HallaTranslator translator;
    app.installTranslator(&translator);

    // fonte padrão estilo Segoe/8.25pt quando disponível
    {
        QFont f = app.font();
        const QStringList preferred = { "Segoe UI", "Noto Sans", "DejaVu Sans",
                                        "Liberation Sans", "Arial" };
        for (const QString& fam : preferred) {
            if (QFontDatabase::families().contains(fam)) { f.setFamily(fam); break; }
        }
        f.setPointSize(qMax(8, S::num("design/fontSize", 9)));
        app.setFont(f);
    }

    // modo de captura de tela (desenvolvimento)
    const QStringList args = app.arguments();
    const int shotIdx = args.indexOf(QStringLiteral("--shot"));
    if (shotIdx >= 0 && shotIdx + 2 < args.size()) {
        return takeShot(app, args.at(shotIdx + 1), args.at(shotIdx + 2));
    }

    MainWindow w;
    w.show();

    // conexão automática (testes): --auto-connect host:porta,apelido[,senha]
    const int acIdx = args.indexOf(QStringLiteral("--auto-connect"));
    if (acIdx >= 0 && acIdx + 1 < args.size()) {
        const QString spec = args.at(acIdx + 1);
        const QStringList parts = spec.split(',');
        const QStringList hp = parts.value(0).split(':');
        const QString host = hp.value(0);
        const quint16 port = hp.value(1) == "9987" ? 9987 : quint16(hp.value(1, "9987").toUShort());
        QTimer::singleShot(300, &w, [&w, host, port, parts] {
            w.connectTo(host, port, parts.value(1, QStringLiteral("HallaUser")),
                        parts.value(2));
        });
        // modo --shot-live <arquivo>: captura a janela após conectar
        const int shotLive = args.indexOf(QStringLiteral("--shot-live"));
        if (shotLive >= 0 && shotLive + 1 < args.size()) {
            const QString out = args.at(shotLive + 1);
            QTimer::singleShot(1800, &w, [&w, out, &app] {
                w.grab().save(out);
                app.quit();
            });
        }
    }

    return app.exec();
}

#include "main.moc"
