#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QTimer>
#include <QPixmap>
#include <QDir>
#include <QTranslator>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>
#include <algorithm>
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
        // 0 = automático: segue o idioma do sistema. As opções 1, 2 e 3
        // são Português, English e Español, respectivamente.
        m_lang = QLocale::system().language();
        switch (S::num(QStringLiteral("app/language"), 0)) {
        case 1: m_lang = QLocale::Portuguese; break;
        case 2: m_lang = QLocale::English; break;
        case 3: m_lang = QLocale::Spanish; break;
        default: break;
        }
        setupTranslations();
        setupWordTranslations();
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
            return translateLoose(key, m_enWords);
        } else if (m_lang == QLocale::Spanish) {
            if (m_es.contains(key)) return m_es.value(key);
            return translateLoose(key, m_esWords);
        }
        return key;
    }

private:
    QLocale::Language m_lang;
    QMap<QString, QString> m_en;
    QMap<QString, QString> m_es;
    QMap<QString, QString> m_enWords;
    QMap<QString, QString> m_esWords;

    QString translateLoose(const QString& source,
                           const QMap<QString, QString>& words) const {
        QString out = source;
        QStringList keys = words.keys();
        std::sort(keys.begin(), keys.end(), [](const QString& a, const QString& b) {
            return a.size() > b.size();
        });
        for (const QString& key : keys) {
            const QString pattern = QStringLiteral("(?<![\\p{L}\\p{N}_])%1(?![\\p{L}\\p{N}_])")
                .arg(QRegularExpression::escape(key));
            out.replace(QRegularExpression(pattern), words.value(key));
        }
        return out;
    }

    void setupWordTranslations() {
        const QList<QPair<QString, QString>> en = {
            {"Configurações", "Settings"}, {"Configuração", "Setting"},
            {"Aplicativo", "Application"}, {"Reprodução", "Playback"},
            {"Capturar", "Capture"}, {"Aparência", "Appearance"},
            {"Notificações", "Notifications"}, {"Teclas de atalho", "Hotkeys"},
            {"Sussurro", "Whisper"}, {"Segurança", "Security"},
            {"Complementos", "Add-ons"}, {"Servidor", "Server"},
            {"servidor", "server"}, {"Servidores", "Servers"},
            {"servidores", "servers"}, {"Canal", "Channel"},
            {"canal", "channel"}, {"Canais", "Channels"},
            {"canais", "channels"}, {"Cliente", "Client"},
            {"cliente", "client"}, {"Clientes", "Clients"},
            {"clientes", "clients"}, {"Usuário", "User"},
            {"usuário", "user"}, {"Usuários", "Users"},
            {"usuários", "users"}, {"Nome", "Name"}, {"nome", "name"},
            {"Endereço", "Address"}, {"endereço", "address"},
            {"Senha", "Password"}, {"senha", "password"},
            {"Mensagem", "Message"}, {"mensagem", "message"},
            {"Descrição", "Description"}, {"descrição", "description"},
            {"Tipo", "Type"}, {"tipo", "type"}, {"Qualidade", "Quality"},
            {"qualidade", "quality"}, {"Permissão", "Permission"},
            {"permissão", "permission"}, {"Permissões", "Permissions"},
            {"permissões", "permissions"}, {"Grupo", "Group"},
            {"grupo", "group"}, {"Grupos", "Groups"}, {"grupos", "groups"},
            {"Padrão", "Default"}, {"padrão", "default"},
            {"Temporário", "Temporary"}, {"temporário", "temporary"},
            {"Permanente", "Permanent"}, {"permanente", "permanent"},
            {"Semi-permanente", "Semi-permanent"}, {"semi-permanente", "semi-permanent"},
            {"Sim", "Yes"}, {"Não", "No"}, {"Cancelar", "Cancel"},
            {"Fechar", "Close"}, {"Aplicar", "Apply"}, {"Salvar", "Save"},
            {"Excluir", "Delete"}, {"Adicionar", "Add"}, {"Atualizar", "Refresh"},
            {"Editar", "Edit"}, {"Remover", "Remove"}, {"Enviar", "Send"},
            {"Baixar", "Download"}, {"Entrar", "Join"}, {"Falar", "Talk"},
            {"Sussurrar", "Whisper"}, {"Moderado", "Moderated"},
            {"moderado", "moderated"}, {"Ausente", "Away"}, {"ausente", "away"},
            {"Mudo", "Muted"}, {"mudo", "muted"}, {"Silenciar", "Mute"},
            {"silenciado", "muted"}, {"Gravação", "Recording"},
            {"gravação", "recording"}, {"Conectar", "Connect"},
            {"conectar", "connect"}, {"Desconectar", "Disconnect"},
            {"desconectar", "disconnect"}, {"Erro", "Error"}, {"erro", "error"},
            {"Motivo", "Reason"}, {"motivo", "reason"}, {"Duração", "Duration"},
            {"duração", "duration"}, {"Poder de fala", "Talk power"},
            {"Banner", "Banner"}, {"banner", "banner"}, {"Imagem", "Image"},
            {"imagem", "image"}, {"Escolher", "Choose"}, {"personalizado", "custom"},
            {"Bem-vindo", "Welcome"}, {"ao", "to"}, {"do", "of"}, {"da", "of"},
            {"no", "on"}, {"em", "in"}, {"para", "for"}, {"com", "with"},
            {"e", "and"}, {"ou", "or"}, {"de", "of"}, {"a", "the"},
            {"o", "the"}, {"um", "a"}, {"uma", "a"}, {"mais", "more"},
            {"menos", "less"}, {"nível", "level"}, {"ativo", "active"},
            {"Inativo", "Inactive"}, {"inativo", "inactive"},
            {"Todos", "All"}, {"todas", "all"}, {"Nenhum", "None"},
            {"Nenhuma", "None"}, {"conectado", "connected"}, {"Conectado", "Connected"},
            {"Desconectado", "Disconnected"}, {"online", "online"},
            {"Tempo", "Time"}, {"tempo", "time"}, {"ativo", "active"},
            {"Cópia", "Copy"}, {"Copiar", "Copy"}, {"Selecionar", "Select"},
            {"Selecionado", "Selected"}, {"Estado", "State"}, {"estado", "state"},
            {"Voltar", "Back"}, {"Próximo", "Next"}, {"Anterior", "Previous"},
            {"Idioma", "Language"}, {"idioma", "language"}, {"Claro", "Light"},
            {"Escuro", "Dark"}, {"Automático", "Automatic"}, {"sistema", "system"},
            {"Recurso", "Feature"}, {"recurso", "feature"}, {"Diversos", "Miscellaneous"},
            {"Opções", "Options"}, {"opções", "options"}, {"Informações", "Information"},
            {"informações", "information"}, {"Desempenho", "Performance"},
            {"Notificação", "Notification"}, {"notificação", "notification"},
            {"Som", "Sound"}, {"som", "sound"}, {"Sons", "Sounds"},
            {"Fones", "Headphones"}, {"Microfone", "Microphone"}, {"microfone", "microphone"},
            {"Alto-falantes", "Speakers"}, {"captura", "capture"},
            {"reprodução", "playback"}, {"voz", "voice"}, {"Voz", "Voice"},
            {"Ativação", "Activation"}, {"ativação", "activation"},
            {"Detecção", "Detection"}, {"detecção", "detection"},
            {"Pressione", "Press"}, {"pressione", "press"}, {"falar", "talk"},
            {"Contínuo", "Continuous"}, {"contínuo", "continuous"},
            {"Sensibilidade", "Sensitivity"}, {"sensibilidade", "sensitivity"},
            {"Dispositivo", "Device"}, {"dispositivo", "device"},
            {"Volume", "Volume"}, {"volume", "volume"}, {"Perfil", "Profile"},
            {"perfil", "profile"}, {"Novo", "New"}, {"novo", "new"},
            {"Feita", "Done"}, {"sucesso", "success"}, {"Sucesso", "Success"},
            {"Falha", "Failure"}, {"falha", "failure"}, {"Reclamações", "Complaints"},
            {"Reclamações", "Complaints"}, {"banidos", "banned users"},
            {"Banidos", "Banned users"}, {"Atualizações", "Updates"},
            {"atualizações", "updates"}, {"Ação", "Action"}, {"ação", "action"},
            {"ocultar", "hide"}, {"Ocultar", "Hide"}, {"Mostrar", "Show"},
            {"mostrar", "show"}, {"Exibir", "Show"}, {"exibir", "show"},
            {"Arquivo", "File"}, {"arquivo", "file"}, {"arquivos", "files"},
            {"Enviar arquivos", "Upload files"}, {"Baixar arquivos", "Download files"},
            {"Tópico", "Topic"}, {"tópico", "topic"}, {"Qualquer", "Any"},
            {"seu", "your"}, {"sua", "your"}, {"Você", "You"}, {"você", "you"},
            {"tem", "has"}, {"está", "is"}, {"estão", "are"}, {"foi", "was"},
            {"servidor virtual", "virtual server"}, {"canal padrão", "default channel"},
            {"Configure", "Configure"}, {"configure", "configure"},
            {"Sussurros", "Whispers"}, {"sussurros", "whispers"},
            {"Áudio", "Audio"}, {"áudio", "audio"}, {"Sistema", "System"},
            {"sistemas", "systems"}, {"Ativar", "Enable"}, {"ativar", "enable"},
            {"Desativar", "Disable"}, {"desativar", "disable"},
            {"Procurar", "Check"}, {"Procurar", "Check"}, {"automaticamente", "automatically"},
            {"compartilhar", "share"}, {"transferência", "transfer"}, {"transferência de arquivos", "file transfer"},
            {"Folha de estilos", "Stylesheet"}, {"folhas de estilos", "stylesheets"},
            {"Suporte", "Support"}, {"suporte", "support"}, {"animado", "animated"},
            {"avatares", "avatars"}, {"imagens", "images"}, {"teclas", "keys"},
            {"atalho", "shortcut"}, {"atalhos", "shortcuts"}, {"recurso", "feature"},
            {"Recurso", "Feature"}, {"configurado", "configured"}, {"configurada", "configured"}
        };
        const QList<QPair<QString, QString>> es = {
            {"Configurações", "Configuración"}, {"Configuração", "Configuración"},
            {"Aplicativo", "Aplicación"}, {"Reprodução", "Reproducción"},
            {"Capturar", "Captura"}, {"Aparência", "Apariencia"},
            {"Notificações", "Notificaciones"}, {"Teclas de atalho", "Atajos de teclado"},
            {"Sussurro", "Susurro"}, {"Segurança", "Seguridad"},
            {"Complementos", "Complementos"}, {"Servidor", "Servidor"},
            {"servidor", "servidor"}, {"Servidores", "Servidores"},
            {"servidores", "servidores"}, {"Canal", "Canal"}, {"canal", "canal"},
            {"Canais", "Canales"}, {"canais", "canales"}, {"Cliente", "Cliente"},
            {"cliente", "cliente"}, {"Clientes", "Clientes"}, {"clientes", "clientes"},
            {"Usuário", "Usuario"}, {"usuário", "usuario"}, {"Usuários", "Usuarios"},
            {"usuários", "usuarios"}, {"Nome", "Nombre"}, {"nome", "nombre"},
            {"Endereço", "Dirección"}, {"endereço", "dirección"}, {"Senha", "Contraseña"},
            {"senha", "contraseña"}, {"Mensagem", "Mensaje"}, {"mensagem", "mensaje"},
            {"Descrição", "Descripción"}, {"descrição", "descripción"}, {"Tipo", "Tipo"},
            {"tipo", "tipo"}, {"Qualidade", "Calidad"}, {"qualidade", "calidad"},
            {"Permissão", "Permiso"}, {"permissão", "permiso"}, {"Permissões", "Permisos"},
            {"permissões", "permisos"}, {"Grupo", "Grupo"}, {"grupo", "grupo"},
            {"Grupos", "Grupos"}, {"grupos", "grupos"}, {"Padrão", "Predeterminado"},
            {"padrão", "predeterminado"}, {"Temporário", "Temporal"}, {"temporário", "temporal"},
            {"Permanente", "Permanente"}, {"permanente", "permanente"},
            {"Semi-permanente", "Semipermanente"}, {"Sim", "Sí"}, {"Não", "No"},
            {"Cancelar", "Cancelar"}, {"Fechar", "Cerrar"}, {"Aplicar", "Aplicar"},
            {"Salvar", "Guardar"}, {"Excluir", "Eliminar"}, {"Adicionar", "Añadir"},
            {"Atualizar", "Actualizar"}, {"Editar", "Editar"}, {"Remover", "Eliminar"},
            {"Enviar", "Enviar"}, {"Baixar", "Descargar"}, {"Entrar", "Entrar"},
            {"Falar", "Hablar"}, {"Sussurrar", "Susurrar"}, {"Moderado", "Moderado"},
            {"moderado", "moderado"}, {"Ausente", "Ausente"}, {"ausente", "ausente"},
            {"Mudo", "Silenciado"}, {"mudo", "silenciado"}, {"Silenciar", "Silenciar"},
            {"silenciado", "silenciado"}, {"Gravação", "Grabación"}, {"gravação", "grabación"},
            {"Conectar", "Conectar"}, {"conectar", "conectar"}, {"Desconectar", "Desconectar"},
            {"desconectar", "desconectar"}, {"Erro", "Error"}, {"erro", "error"},
            {"Motivo", "Motivo"}, {"motivo", "motivo"}, {"Duração", "Duración"},
            {"duração", "duración"}, {"Poder de fala", "Poder de habla"},
            {"Imagem", "Imagen"}, {"imagem", "imagen"}, {"Escolher", "Elegir"},
            {"personalizado", "personalizado"}, {"Bem-vindo", "Bienvenido"},
            {"ao", "al"}, {"do", "del"}, {"da", "de la"}, {"no", "en el"},
            {"em", "en"}, {"para", "para"}, {"com", "con"}, {"e", "y"}, {"ou", "o"},
            {"de", "de"}, {"a", "el"}, {"o", "el"}, {"um", "un"}, {"uma", "una"},
            {"mais", "más"}, {"menos", "menos"}, {"nível", "nivel"},
            {"ativo", "activo"}, {"Inativo", "Inactivo"}, {"inativo", "inactivo"},
            {"Todos", "Todos"}, {"todas", "todas"}, {"Nenhum", "Ninguno"},
            {"Nenhuma", "Ninguna"}, {"conectado", "conectado"}, {"Conectado", "Conectado"},
            {"Desconectado", "Desconectado"}, {"Tempo", "Tiempo"}, {"tempo", "tiempo"},
            {"Estado", "Estado"}, {"estado", "estado"}, {"Voltar", "Atrás"},
            {"Próximo", "Siguiente"}, {"Anterior", "Anterior"}, {"Idioma", "Idioma"},
            {"idioma", "idioma"}, {"Claro", "Claro"}, {"Escuro", "Oscuro"},
            {"Automático", "Automático"}, {"sistema", "sistema"}, {"Informações", "Información"},
            {"informações", "información"}, {"Diversos", "Varios"}, {"Opções", "Opciones"},
            {"opções", "opciones"}, {"Som", "Sonido"}, {"som", "sonido"},
            {"Sons", "Sonidos"}, {"Fones", "Auriculares"}, {"Microfone", "Micrófono"},
            {"microfone", "micrófono"}, {"Alto-falantes", "Altavoces"}, {"captura", "captura"},
            {"reprodução", "reproducción"}, {"voz", "voz"}, {"Voz", "Voz"},
            {"Ativação", "Activación"}, {"ativação", "activación"}, {"Detecção", "Detección"},
            {"detecção", "detección"}, {"Pressione", "Presione"}, {"pressione", "presione"},
            {"falar", "hablar"}, {"Contínuo", "Continuo"}, {"contínuo", "continuo"},
            {"Sensibilidade", "Sensibilidad"}, {"sensibilidade", "sensibilidad"},
            {"Dispositivo", "Dispositivo"}, {"dispositivo", "dispositivo"},
            {"Perfil", "Perfil"}, {"perfil", "perfil"}, {"Novo", "Nuevo"}, {"novo", "nuevo"},
            {"Sucesso", "Éxito"}, {"sucesso", "éxito"}, {"Falha", "Fallo"}, {"falha", "fallo"},
            {"Reclamações", "Quejas"}, {"banidos", "baneados"}, {"Banidos", "Baneados"},
            {"Atualizações", "Actualizaciones"}, {"atualizações", "actualizaciones"},
            {"Ação", "Acción"}, {"ação", "acción"}, {"Ocultar", "Ocultar"}, {"ocultar", "ocultar"},
            {"Mostrar", "Mostrar"}, {"mostrar", "mostrar"}, {"Exibir", "Mostrar"},
            {"exibir", "mostrar"}, {"Arquivo", "Archivo"}, {"arquivo", "archivo"},
            {"arquivos", "archivos"}, {"Tópico", "Tema"}, {"tópico", "tema"},
            {"seu", "tu"}, {"sua", "tu"}, {"Você", "Tú"}, {"você", "tú"},
            {"tem", "tiene"}, {"está", "está"}, {"estão", "están"}, {"foi", "fue"},
            {"servidor virtual", "servidor virtual"}, {"canal padrão", "canal predeterminado"},
            {"Configure", "Configura"}, {"configure", "configura"},
            {"Sussurros", "Susurros"}, {"sussurros", "susurros"},
            {"Áudio", "Audio"}, {"áudio", "audio"}, {"Sistema", "Sistema"},
            {"sistemas", "sistemas"}, {"Ativar", "Activar"}, {"ativar", "activar"},
            {"Desativar", "Desactivar"}, {"desativar", "desactivar"},
            {"Procurar", "Buscar"}, {"automaticamente", "automáticamente"},
            {"compartilhar", "compartir"}, {"transferência", "transferencia"},
            {"transferência de arquivos", "transferencia de archivos"},
            {"Folha de estilos", "Hoja de estilos"}, {"folhas de estilos", "hojas de estilos"},
            {"Suporte", "Soporte"}, {"suporte", "soporte"}, {"animado", "animado"},
            {"avatares", "avatares"}, {"imagens", "imágenes"}, {"teclas", "teclas"},
            {"atalho", "atajo"}, {"atalhos", "atajos"}, {"recurso", "recurso"},
            {"Recurso", "Recurso"}, {"configurado", "configurado"}, {"configurada", "configurada"}
        };
        for (const auto& pair : en) m_enWords.insert(pair.first, pair.second);
        for (const auto& pair : es) m_esWords.insert(pair.first, pair.second);
    }

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

        // Termos recentes do banner personalizado e dos indicadores de
        // estado. O fallback por palavras cobre também as mensagens novas
        // adicionadas nas páginas de opções.
        m_en["Imagem do banner:"] = "Banner image:";
        m_en["Escolher imagem..."] = "Choose image...";
        m_en["Selecionar imagem do banner"] = "Select banner image";
        m_en["Banner padrão"] = "Default banner";
        m_en["Banner personalizado"] = "Custom banner";
        m_en["Remover"] = "Remove";
        m_en["Não foi possível abrir essa imagem."] = "Could not open this image.";
        m_en["A imagem precisa ter no máximo 512 KiB."] = "The image must be no larger than 512 KiB.";
        m_es["Imagem do banner:"] = "Imagen del banner:";
        m_es["Escolher imagem..."] = "Elegir imagen...";
        m_es["Selecionar imagem do banner"] = "Seleccionar imagen del banner";
        m_es["Banner padrão"] = "Banner predeterminado";
        m_es["Banner personalizado"] = "Banner personalizado";
        m_es["Não foi possível abrir essa imagem."] = "No se pudo abrir esta imagen.";
        m_es["A imagem precisa ter no máximo 512 KiB."] = "La imagen no puede superar 512 KiB.";
    }
};

int main(int argc, char* argv[]) {
    Q_INIT_RESOURCE(halla);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Halla"));
    QApplication::setOrganizationName(QStringLiteral("Halla"));
    QApplication::setApplicationDisplayName(QStringLiteral("Halla"));
    QApplication::setApplicationVersion(QString::fromUtf8(halla::kAppVersion));
    QApplication::setWindowIcon(QIcon(HIcons::appIcon(256)));

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
