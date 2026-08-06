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
            {"Configure", "Configure"},
            {"Opções gerais do aplicativo", "General application options"},
            {"Configure o sistema de reprodução de áudio", "Configure the audio playback system"},
            {"Configure o sistema de captura de áudio", "Configure the audio capture system"},
            {"Configure a aparência", "Configure the appearance"},
            {"Sons e avisos de eventos", "Event sounds and alerts"},
            {"Configure teclas de atalho", "Configure hotkeys"},
            {"Configure o recurso de sussurros", "Configure whisper features"},
            {"Identidade e segurança", "Identity and security"},
            {"Extensões e pacotes do cliente", "Client extensions and packages"},
            {"Restaurar as conexões da sessão anterior", "Restore connections from the previous session"},
            {"Procurar atualizações automaticamente", "Check for updates automatically"},
            {"Fechar para a bandeja do sistema", "Close to the system tray"},
            {"Confirmar ao sair estando conectado", "Confirm when exiting while connected"},
            {"Expandir todos os canais ao fazer login", "Expand all channels on login"},
            {"Expandir canais até este nível:", "Expand channels to this level:"},
            {"Expandir o próprio canal ao fazer login", "Expand your own channel on login"},
            {"Classificar clientes abaixo dos canais", "Sort clients below channels"},
            {"Exibir bandeira de país nos clientes", "Show country flags on clients"},
            {"Exibir ícones de emblema nos clientes", "Show badge icons on clients"},
            {"Exibir ícones de grupo nos menus de contexto", "Show group icons in context menus"},
            {"Ocultar grupos inacessíveis nos menus de contexto", "Hide inaccessible groups in context menus"},
            {"Mostrar número de clientes ao lado dos canais", "Show client count next to channels"},
            {"Mostrar mini-ícones de estado dos clientes", "Show client status mini-icons"},
            {"Mostrar mensagem de ausência ao lado do apelido", "Show the away message beside the nickname"},
            {"Mostrar dica de ferramenta ao passar o mouse", "Show tooltips when hovering"},
            {"Ativar avatares animados", "Enable animated avatars"},
            {"Ativar imagens animadas", "Enable animated images"},
            {"Atribua permissões a grupos e clientes", "Assign permissions to groups and clients"},
            {"Banimentos ativos neste servidor", "Active bans on this server"},
            {"Crie grupos, edite permissões e atribua usuários", "Create groups, edit permissions and assign users"},
            {"Atribuir grupo a cliente conectado", "Assign group to connected client"},
            {"Atribuição", "Assignment"}, {"Apelido fonético:", "Phonetic nickname:"},
            {"Digite o endereço do servidor.", "Enter the server address."},
            {"Digite um apelido.", "Enter a nickname."},
            {"Digite um endereço de servidor, apelido e, se necessário, a senha do servidor.", "Enter a server address, nickname and, if needed, the server password."},
            {"Mensagens deixadas para você enquanto estava ausente", "Messages left for you while you were away"},
            {"Mensagem entregue. Será mostrada quando o usuário se conectar.", "Message delivered. It will be shown when the user connects."},
            {"A lista de banidos está disponível apenas conectado a um Halla Server.", "The ban list is available only while connected to a Halla Server."},
            {"Reclamações estão disponíveis apenas conectado a um Halla Server.", "Complaints are available only while connected to a Halla Server."},
            {"Use uma imagem PNG, JPEG, GIF ou WebP de até 512 KiB.", "Use a PNG, JPEG, GIF or WebP image up to 512 KiB."},
            {"Imagem do banner:", "Banner image:"}, {"Escolher imagem...", "Choose image..."},
            {"Selecionar imagem do banner", "Select banner image"}, {"Banner padrão", "Default banner"},
            {"Banner personalizado", "Custom banner"}, {"Não foi possível abrir essa imagem.", "Could not open this image."},
            {"A imagem precisa ter no máximo 512 KiB.", "The image must be no larger than 512 KiB."},
            {"Emitir sinal sonoro ao falar", "Play a sound cue when speaking"},
            {"Emitir ao:", "Play when:"}, {"Pressione para Falar", "Push-to-Talk"},
            {"Atividade de Voz", "Voice Activity"}, {"Outros usuários", "Other users"},
            {"Pesquisar arquivo", "Browse for file"}, {"Nenhum arquivo selecionado", "No file selected"},
            {"Os sinais locais acompanham o modo escolhido.", "Local cues follow the selected mode."},
            {"Marque", "Check"}, {"segurar", "hold"}, {"soltar", "release"},
            {"Atraso ao soltar a tecla do Push-to-Talk", "Push-to-Talk release delay"},
            {"Redução de ruído", "Noise reduction"}, {"Cancelamento do eco", "Echo cancellation"},
            {"Processamento digital de sinais", "Digital signal processing"},
            {"Redução de eco (Ducking):", "Echo reduction (ducking):"},
            {"Sistema de permissões avançado", "Advanced permission system"},
            {"Obter mais folhas de estilos && ícones", "Get more stylesheets && icons"},
            {"Opções de reprodução...", "Playback options..."},
            {"Sinal sonoro de sussurro", "Whisper sound cue"},
            {"Você não tem permissão", "You do not have permission"},
            {"não tem permissão", "does not have permission"}, {"Não foi possível", "Could not"},
            {"Não é possível", "Cannot"}, {"Não encontrado", "Not found"},
            {"excede o limite", "exceeds the limit"}, {"excede", "exceeds"}, {"limite", "limit"},
            {"enviado", "sent"}, {"enviada", "sent"}, {"enviados", "sent"},
            {"definido", "set"}, {"definida", "set"}, {"criado", "created"},
            {"criada", "created"}, {"editado", "edited"}, {"editada", "edited"},
            {"alterado", "changed"}, {"alterada", "changed"}, {"removido", "removed"},
            {"removida", "removed"}, {"excluído", "deleted"}, {"excluída", "deleted"},
            {"protegido", "protected"}, {"protegida", "protected"}, {"cheio", "full"},
            {"vazia", "empty"}, {"vazio", "empty"}, {"conectados", "connected"},
            {"conectadas", "connected"}, {"disponível", "available"}, {"disponíveis", "available"},
            {"necessário", "required"}, {"necessária", "required"}, {"permitir", "allow"},
            {"permitido", "allowed"}, {"negada", "denied"}, {"concedida", "granted"},
            {"convidado", "guest"}, {"Convidado", "Guest"}, {"Admin do servidor", "Server admin"},
            {"admin", "admin"}, {"normal", "normal"}, {"guest", "guest"},
            {"Poder", "Power"}, {"poder", "power"}, {"Valor", "Value"}, {"valor", "value"},
            {"Ativo", "Active"}, {"ativo", "active"}, {"Inativo", "Inactive"},
            {"inativo", "inactive"}, {"Talk power", "Talk power"}, {"Direct Sound", "Direct Sound"},
            {"MODO", "MODE"}, {"modo", "mode"}, {"Ajuste", "Adjustment"}, {"ajuste", "adjustment"},
            {"Baixo", "Low"}, {"baixo", "low"}, {"Alto", "High"}, {"alto", "high"},
            {"Quieto", "Quiet"}, {"Ruído", "Noise"}, {"ruído", "noise"}, {"fundo", "background"},
            {"Atenuação", "Attenuation"}, {"atenuação", "attenuation"}, {"digitação", "typing"},
            {"cliques", "clicks"}, {"Sempre", "Always"}, {"definir", "set"}, {"posições", "positions"},
            {"quando", "when"}, {"houver", "there is"}, {"permitir", "allow"}, {"histórico", "history"},
            {"privada", "private"}, {"privado", "private"}, {"trocar", "switch"}, {"recebimento", "receiving"},
            {"configuração", "configuration"}, {"configurada", "configured"}, {"momento", "moment"},
            {"vinculados", "linked"}, {"vincular", "link"}, {"Desvincular", "Unlink"},
            {"subcanal", "subchannel"}, {"subcanais", "subchannels"}, {"área", "area"},
            {"painel", "panel"}, {"painéis", "panels"}, {"linha", "line"}, {"linhas", "lines"},
            {"botão", "button"}, {"botões", "buttons"}, {"lado", "side"}, {"direita", "right"},
            {"esquerda", "left"}, {"central", "center"}, {"superior", "top"}, {"inferior", "bottom"},
            {"principal", "main"}, {"relatório", "report"}, {"registro", "log"}, {"logs", "logs"},
            {"Cache local limpo pelo usuário", "Local cache cleared by the user"},
            {"O arquivo excede o limite de tamanho de 64 KiB.", "The file exceeds the 64 KiB size limit."},
            {"Grupos internos não podem ser excluídos.", "Built-in groups cannot be deleted."},
            {"Grupos internos não podem ser renomeados.", "Built-in groups cannot be renamed."},
            {"Excluir TODAS as reclamações deste servidor?", "Delete ALL complaints from this server?"},
            {"configure", "configure"},
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
        const QList<QPair<QString, QString>> enExtra = {
            {"Si mesmo", "Myself"}, {"Ferramentas", "Tools"}, {"Principal", "Main"},
            {"Conexões", "Connections"}, {"Conexão", "Connection"}, {"Marcadores", "Bookmarks"},
            {"Permissões", "Permissions"}, {"Permissão", "Permission"}, {"Nenhum servidor", "No server"},
            {"Conectar a um servidor", "Connect to a server"}, {"Gerenciar favoritos", "Manage bookmarks"},
            {"Não conectado — use Conexões ▸ Conectar… para entrar em um servidor", "Not connected — use Connections ▸ Connect... to join a server"},
            {"Bem-vindo ao Halla!  •  Cliente de comunicação de voz  •  github.com/GroupHalla/Halla", "Welcome to Halla!  •  Voice communication client  •  github.com/GroupHalla/Halla"},
            {"None server", "No server"}, {"Conectado ao servidor", "Connected to server"},
            {"Conectado ao servidor: %1", "Connected to server: %1"}, {"Halla encerrado", "Halla closed"},
            {"Halla %1 iniciado", "Halla %1 started"}, {"Desconectado do servidor", "Disconnected from server"},
            {"Desconectado do servidor.", "Disconnected from server."}, {"Desconectado de %1", "Disconnected from %1"},
            {"Mensagens offline", "Offline messages"}, {"Listas de sussurro...", "Whisper lists..."},
            {"Transferência de arquivos...", "File transfer..."}, {"Registro do cliente", "Client log"},
            {"Identidades...", "Identities..."}, {"Contatos...", "Contacts..."}, {"Lista de banidos", "Ban list"},
            {"Lista de banidos...", "Ban list..."}, {"Reclamações", "Complaints"}, {"Reclamações...", "Complaints..."},
            {"Grupos de servidores...", "Server groups..."}, {"Mostrar permissões do usuário...", "Show user permissions..."},
            {"Adicionar aos marcadores...", "Add to bookmarks..."}, {"Conectar a todos os favoritos", "Connect to all bookmarks"},
            {"Nenhum favorito", "No bookmarks"}, {"Nenhuma notificação nova", "No new notifications"},
            {"Notificações", "Notifications"}, {"Notificação", "Notification"}, {"Atualização", "Update"},
            {"Nova atualização disponível", "New update available"}, {"Download concluído", "Download complete"},
            {"Erro de download", "Download error"}, {"Baixando atualização", "Downloading update"},
            {"Baixando Halla v%1...", "Downloading Halla v%1..."}, {"Uma nova versão (%1) está disponível!\\nDeseja baixar e instalar agora?", "A new version (%1) is available!\\nDownload and install now?"},
            {"Você já está usando a versão mais recente do Halla.", "You are already using the latest Halla version."},
            {"Não foi possível registrar a tecla PTT: %1", "Could not register the PTT key: %1"},
            {"Não foi possível salvar o arquivo de atualização no diretório temporário.", "Could not save the update file in the temporary directory."},
            {"O Halla continua em execução na bandeja do sistema.", "Halla is still running in the system tray."},
            {"O download foi concluído com sucesso. O instalador será executado agora.", "The download completed successfully. The installer will run now."},
            {"Você ainda está conectado a servidores.\\nDeseja realmente sair?", "You are still connected to servers.\\nDo you really want to exit?"},
            {"Você entrou no canal \\\"%1\\\".", "You joined channel \\\"%1\\\"."},
            {"Canal padrão", "Default channel"}, {"Canal padrão", "Default channel"},
            {"HallaUser", "HallaUser"}, {"Admin do servidor", "Server administrator"},
            {"Atribua permissões a grupos e clientes", "Assign permissions to groups and clients"},
            {"Clientes:", "Clients:"}, {"Grupos de servidores:", "Server groups:"}, {"Atribuído", "Assigned"},
            {"Convidado", "Guest"}, {"Normal", "Normal"}, {"Admin", "Admin"}, {"ID único", "Unique ID"},
            {"Expira em", "Expires"}, {"Banimentos ativos neste servidor", "Active bans on this server"},
            {"Reclamações registradas pelos usuários", "Complaints registered by users"},
            {"Limpar reclamações", "Clear complaints"}, {"Limpar todas", "Clear all"}, {"Remover banimento", "Remove ban"},
            {"Remover do usuário selecionado", "Remove from selected user"}, {"Grupo interno", "Built-in group"},
            {"Grupos internos (convidado, normal, admin) não podem ser excluídos.", "Built-in groups (guest, normal, admin) cannot be deleted."},
            {"Grupos internos não podem ser renomeados.", "Built-in groups cannot be renamed."},
            {"Novo grupo", "New group"}, {"Renomear grupo", "Rename group"}, {"Excluir grupo", "Delete group"},
            {"Nome do grupo:", "Group name:"}, {"Prefixo/Sigla:", "Prefix/abbreviation:"},
            {"Ordem de Hierarquia:", "Hierarchy order:"}, {"Ícone do Cargo:", "Role icon:"},
            {"Propriedades do Cargo", "Role properties"}, {"Aplicar permissões", "Apply permissions"},
            {"Atribuir grupo a cliente conectado", "Assign group to connected client"},
            {"Suas permissões neste servidor (grupo: %1)", "Your permissions on this server (group: %1)"},
            {"Poder de fala   (talkPower)", "Talk power (talkPower)"}, {"concedida", "granted"}, {"negada", "denied"},
            {"Atribuição", "Assignment"}, {"Destinatário:", "Recipient:"}, {"De", "From"}, {"Por", "By"},
            {"Mensagem entregue. Será mostrada quando o usuário se conectar.", "Message delivered. It will be shown when the user connects."},
            {"Nova mensagem offline", "New offline message"}, {"Mensagem:", "Message:"}, {"Mensagens offline", "Offline messages"},
            {"Adicione, edite e conecte-se aos seus servidores favoritos", "Add, edit and connect to your favorite servers"},
            {"Gerenciar favoritos", "Manage bookmarks"}, {"Novo favorito", "New bookmark"}, {"Favorito", "Bookmark"},
            {"Rótulo:", "Label:"}, {"Apelido:", "Nickname:"}, {"Senha do servidor:", "Server password:"},
            {"Conectar automaticamente ao iniciar", "Connect automatically on startup"}, {"Porta:", "Port:"},
            {"Nome do canal:", "Channel name:"}, {"Configure as propriedades do canal", "Configure channel properties"},
            {"Propriedades", "Properties"}, {"Permissões", "Permissions"}, {"Grupos ativos", "Active groups"},
            {"Negar", "Deny"}, {"Permitir", "Allow"}, {"Entrar no canal", "Join channel"},
            {"Falar no canal", "Talk in channel"}, {"Sussurrar neste canal", "Whisper in this channel"},
            {"Mensagem de texto", "Text message"}, {"Enviar arquivos", "Upload files"}, {"Baixar arquivos", "Download files"},
            {"Moderado (precisa de poder de fala)", "Moderated (requires talk power)"}, {"Tipo do canal:", "Channel type:"},
            {"Máx. de clientes:", "Max clients:"}, {"Classificar abaixo de:", "Sort below:"}, {"Ocultar símbolo do canal", "Hide channel symbol"},
            {"Bitrate do codec:", "Codec bitrate:"}, {"Qualidade do codec:", "Codec quality:"},
            {"Descrição:", "Description:"}, {"Tópico:", "Topic:"}, {"Senha:", "Password:"},
            {"Canal padrão", "Default channel"}, {"Temporário", "Temporary"}, {"Semi-permanente", "Semi-permanent"},
            {"Permanente", "Permanent"}, {"ilimitado", "unlimited"}, {"Codec:", "Codec:"},
            {"Apelido fonético:", "Phonetic nickname:"}, {"Endereço do servidor:", "Server address:"},
            {"Endereço do servidor", "Server address"}, {"Digite o endereço do servidor.", "Enter the server address."},
            {"Digite um apelido.", "Enter a nickname."}, {"Mais >>", "More >>"}, {"Menos <<", "Less <<"},
            {"Perfil de captura:", "Capture profile:"}, {"Perfil de reprodução:", "Playback profile:"},
            {"Alto-falantes", "Speakers"}, {"Volume de reprodução", "Playback volume"}, {"Dispositivo de reprodução:", "Playback device:"},
            {"Dispositivo de captura:", "Capture device:"}, {"Usar o melhor modo automaticamente", "Use the best mode automatically"},
            {"Ativação de voz", "Voice activation"}, {"Pressionar para falar (PTT)", "Push-to-talk (PTT)"},
            {"Transmissão contínua ativada", "Continuous transmission enabled"}, {"Transmissão contínua desativada", "Continuous transmission disabled"},
            {"Sinal sonoro de sussurro", "Whisper sound cue"}, {"Arquivo de áudio", "Audio file"}, {"Outros usuários", "Other users"},
            {"Sinais sonoros", "Sound cues"}, {"Ativo", "Active"}, {"Inativo", "Inactive"}, {"Sussurro", "Whisper"},
            {"Escolher arquivo", "Choose file"}, {"Nenhum arquivo selecionado", "No file selected"},
            {"Emitir sinal sonoro ao falar", "Play sound cue when speaking"}, {"Emitir ao:", "Play when:"},
            {"Pressione para Falar", "Push-to-Talk"}, {"Atividade de Voz", "Voice Activity"},
            {"Redução de ruído", "Noise reduction"}, {"Cancelamento do eco", "Echo cancellation"},
            {"Redução de eco (Ducking):", "Echo reduction (ducking):"}, {"Sensibilidade do microfone", "Microphone sensitivity"},
            {"Processamento digital de sinais", "Digital signal processing"}, {"Atraso ao soltar", "Release delay"},
            {"Lista de sussurros", "Whisper list"}, {"Clientes & canais", "Clients & channels"},
            {"Pesquisar...", "Search..."}, {"Ver tudo", "View all"}, {"Filtros", "Filters"},
            {"Nenhum item neste painel...", "No items in this panel..."}, {"Salvar", "Save"}, {"Aplicar", "Apply"},
            {"Atualizar", "Refresh"}, {"Recarregar", "Reload"}, {"Fechar", "Close"}, {"Cancelar", "Cancel"},
            {"OK", "OK"}, {"Sobre o Halla", "About Halla"}, {"Cliente de comunicação de voz", "Voice communication client"},
            {"Todos os direitos reservados.", "All rights reserved."}, {"Compilado em:", "Compiled on:"},
            {"Registro do cliente", "Client log"}, {"Hora", "Time"}, {"Nível", "Level"}, {"Mensagem", "Message"},
            {"Ativado", "Enabled"}, {"Desativado", "Disabled"}, {"Acesso negado", "Access denied"},
            {"Conexão", "Connection"}, {"Conectado", "Connected"}, {"Desconectado", "Disconnected"},
            {"Conectado como %1", "Connected as %1"}, {"Ping atual:", "Current ping:"}, {"Perda de pacotes:", "Packet loss:"},
            {"Você", "You"}, {"Entrou no canal", "Joined channel"}, {"saiu do canal", "left the channel"},
            {"cutucou", "poked"}, {"foi expulso", "was kicked"}, {"foi banido", "was banned"},
            {"aguarde", "please wait"}, {"instalado", "installed"}, {"instalador", "installer"},
            {"diretório", "directory"}, {"temporário", "temporary"}, {"abertas", "open"}, {"fechado", "closed"},
            {"fechada", "closed"}, {"excluído", "deleted"}, {"excluída", "deleted"}, {"vinculado", "linked"},
            {"vinculados", "linked"}, {"reordenar", "reorder"}, {"ordenação", "ordering"}, {"posição", "position"},
            {"Operador de canal: quem cria o canal gerencia", "Channel operator: the creator manages the channel"},
            {"membros temporários de canais seguem o grupo global", "temporary channel members follow the global group"}
        };
        const QList<QPair<QString, QString>> esExtra = {
            {"Si mesmo", "Yo"}, {"Ferramentas", "Herramientas"}, {"Principal", "Principal"},
            {"Conexões", "Conexiones"}, {"Conexão", "Conexión"}, {"Marcadores", "Marcadores"},
            {"Permissões", "Permisos"}, {"Permissão", "Permiso"}, {"Nenhum servidor", "Ningún servidor"},
            {"Conectar a um servidor", "Conectar a un servidor"}, {"Gerenciar favoritos", "Administrar favoritos"},
            {"Não conectado — use Conexões ▸ Conectar… para entrar em um servidor", "No conectado — usa Conexiones ▸ Conectar... para entrar en un servidor"},
            {"Bem-vindo ao Halla!  •  Cliente de comunicação de voz  •  github.com/GroupHalla/Halla", "Bienvenido a Halla!  •  Cliente de comunicación por voz  •  github.com/GroupHalla/Halla"},
            {"None server", "Ningún servidor"}, {"Conectado ao servidor", "Conectado al servidor"},
            {"Conectado ao servidor: %1", "Conectado al servidor: %1"}, {"Halla encerrado", "Halla cerrado"},
            {"Halla %1 iniciado", "Halla %1 iniciado"}, {"Desconectado do servidor", "Desconectado del servidor"},
            {"Desconectado do servidor.", "Desconectado del servidor."}, {"Desconectado de %1", "Desconectado de %1"},
            {"Mensagens offline", "Mensajes sin conexión"}, {"Listas de sussurro...", "Listas de susurro..."},
            {"Transferência de arquivos...", "Transferencia de archivos..."}, {"Registro do cliente", "Registro del cliente"},
            {"Identidades...", "Identidades..."}, {"Contatos...", "Contactos..."}, {"Lista de banidos", "Lista de baneados"},
            {"Lista de banidos...", "Lista de baneados..."}, {"Reclamações", "Quejas"}, {"Reclamações...", "Quejas..."},
            {"Grupos de servidores...", "Grupos de servidores..."}, {"Mostrar permissões do usuário...", "Mostrar permisos del usuario..."},
            {"Adicionar aos marcadores...", "Añadir a marcadores..."}, {"Conectar a todos os favoritos", "Conectar a todos los favoritos"},
            {"Nenhum favorito", "Ningún favorito"}, {"Nenhuma notificação nova", "Ninguna notificación nueva"},
            {"Notificações", "Notificaciones"}, {"Notificação", "Notificación"}, {"Atualização", "Actualización"},
            {"Nova atualização disponível", "Nueva actualización disponible"}, {"Download concluído", "Descarga completada"},
            {"Erro de download", "Error de descarga"}, {"Baixando atualização", "Descargando actualización"},
            {"Baixando Halla v%1...", "Descargando Halla v%1..."},
            {"Você já está usando a versão mais recente do Halla.", "Ya estás usando la versión más reciente de Halla."},
            {"Você ainda está conectado a servidores.\\nDeseja realmente sair?", "Todavía estás conectado a servidores.\\n¿Realmente deseas salir?"},
            {"Você entrou no canal \\\"%1\\\".", "Entraste al canal \\\"%1\\\"."},
            {"Canal padrão", "Canal predeterminado"}, {"Admin do servidor", "Administrador del servidor"},
            {"Atribua permissões a grupos e clientes", "Asigna permisos a grupos y clientes"},
            {"Clientes:", "Clientes:"}, {"Grupos de servidores:", "Grupos de servidores:"}, {"Atribuído", "Asignado"},
            {"Convidado", "Invitado"}, {"Normal", "Normal"}, {"Admin", "Administrador"}, {"ID único", "ID único"},
            {"Expira em", "Expira"}, {"Banimentos ativos neste servidor", "Baneos activos en este servidor"},
            {"Reclamações registradas pelos usuários", "Quejas registradas por los usuarios"},
            {"Limpar reclamações", "Limpiar quejas"}, {"Limpar todas", "Limpiar todas"}, {"Remover banimento", "Eliminar baneo"},
            {"Novo grupo", "Nuevo grupo"}, {"Renomear grupo", "Renombrar grupo"}, {"Excluir grupo", "Eliminar grupo"},
            {"Nome do grupo:", "Nombre del grupo:"}, {"Prefixo/Sigla:", "Prefijo/abreviatura:"},
            {"Ordem de Hierarquia:", "Orden jerárquico:"}, {"Ícone do Cargo:", "Icono del cargo:"},
            {"Propriedades do Cargo", "Propiedades del cargo"}, {"Aplicar permissões", "Aplicar permisos"},
            {"Suas permissões neste servidor (grupo: %1)", "Tus permisos en este servidor (grupo: %1)"},
            {"Poder de fala   (talkPower)", "Poder de habla (talkPower)"}, {"concedida", "concedido"}, {"negada", "denegado"},
            {"Destinatário:", "Destinatario:"}, {"De", "De"}, {"Por", "Por"},
            {"Mensagem entregue. Será mostrada quando o usuário se conectar.", "Mensaje entregado. Se mostrará cuando el usuario se conecte."},
            {"Adicione, edite e conecte-se aos seus servidores favoritos", "Añade, edita y conéctate a tus servidores favoritos"},
            {"Novo favorito", "Nuevo favorito"}, {"Favorito", "Favorito"}, {"Rótulo:", "Etiqueta:"},
            {"Apelido:", "Apodo:"}, {"Senha do servidor:", "Contraseña del servidor:"}, {"Porta:", "Puerto:"},
            {"Conectar automaticamente ao iniciar", "Conectar automáticamente al iniciar"},
            {"Nome do canal:", "Nombre del canal:"}, {"Configure as propriedades do canal", "Configura las propiedades del canal"},
            {"Propriedades", "Propiedades"}, {"Grupos ativos", "Grupos activos"}, {"Negar", "Denegar"}, {"Permitir", "Permitir"},
            {"Entrar no canal", "Entrar al canal"}, {"Falar no canal", "Hablar en el canal"}, {"Sussurrar neste canal", "Susurrar en este canal"},
            {"Mensagem de texto", "Mensaje de texto"}, {"Enviar arquivos", "Subir archivos"}, {"Baixar arquivos", "Descargar archivos"},
            {"Moderado (precisa de poder de fala)", "Moderado (requiere poder de habla)"}, {"Tipo do canal:", "Tipo de canal:"},
            {"Máx. de clientes:", "Máx. de clientes:"}, {"Classificar abaixo de:", "Ordenar debajo de:"}, {"Ocultar símbolo do canal", "Ocultar símbolo del canal"},
            {"Bitrate do codec:", "Bitrate del códec:"}, {"Qualidade do codec:", "Calidad del códec:"},
            {"Descrição:", "Descripción:"}, {"Tópico:", "Tema:"}, {"Senha:", "Contraseña:"},
            {"ilimitado", "ilimitado"}, {"Apelido fonético:", "Apodo fonético:"}, {"Endereço do servidor:", "Dirección del servidor:"},
            {"Endereço do servidor", "Dirección del servidor"}, {"Digite o endereço do servidor.", "Escribe la dirección del servidor."},
            {"Digite um apelido.", "Escribe un apodo."}, {"Mais >>", "Más >>"}, {"Menos <<", "Menos <<"},
            {"Perfil de captura:", "Perfil de captura:"}, {"Perfil de reprodução:", "Perfil de reproducción:"},
            {"Alto-falantes", "Altavoces"}, {"Dispositivo de reprodução:", "Dispositivo de reproducción:"},
            {"Dispositivo de captura:", "Dispositivo de captura:"}, {"Usar o melhor modo automaticamente", "Usar el mejor modo automáticamente"},
            {"Ativação de voz", "Activación de voz"}, {"Pressionar para falar (PTT)", "Presionar para hablar (PTT)"},
            {"Transmissão contínua ativada", "Transmisión continua activada"}, {"Transmissão contínua desativada", "Transmisión continua desactivada"},
            {"Sinal sonoro de sussurro", "Señal sonora de susurro"}, {"Arquivo de áudio", "Archivo de audio"}, {"Outros usuários", "Otros usuarios"},
            {"Sinais sonoros", "Señales sonoras"}, {"Ativo", "Activo"}, {"Inativo", "Inactivo"}, {"Sussurro", "Susurro"},
            {"Escolher arquivo", "Elegir archivo"}, {"Nenhum arquivo selecionado", "Ningún archivo seleccionado"},
            {"Emitir sinal sonoro ao falar", "Emitir señal sonora al hablar"}, {"Emitir ao:", "Emitir al:"},
            {"Pressione para Falar", "Presionar para hablar"}, {"Atividade de Voz", "Actividad de voz"},
            {"Redução de ruído", "Reducción de ruido"}, {"Cancelamento do eco", "Cancelación de eco"},
            {"Redução de eco (Ducking):", "Reducción de eco (ducking):"}, {"Sensibilidade do microfone", "Sensibilidad del micrófono"},
            {"Processamento digital de sinais", "Procesamiento digital de señales"}, {"Lista de sussurros", "Lista de susurros"},
            {"Clientes & canais", "Clientes y canales"}, {"Pesquisar...", "Buscar..."}, {"Ver tudo", "Ver todo"},
            {"Nenhum item neste painel...", "No hay elementos en este panel..."}, {"Sobre o Halla", "Acerca de Halla"},
            {"Cliente de comunicação de voz", "Cliente de comunicación por voz"}, {"Todos os direitos reservados.", "Todos los derechos reservados."},
            {"Compilado em:", "Compilado el:"}, {"Registro do cliente", "Registro del cliente"}, {"Hora", "Hora"},
            {"Nível", "Nivel"}, {"Acesso negado", "Acceso denegado"}, {"Conectado como %1", "Conectado como %1"},
            {"Você", "Tú"}, {"saiu do canal", "salió del canal"}, {"cutucou", "dio un toque"},
            {"foi expulso", "fue expulsado"}, {"foi banido", "fue baneado"}, {"instalador", "instalador"},
            {"diretório", "directorio"}, {"temporário", "temporal"}, {"abertas", "abiertas"}, {"fechado", "cerrado"},
            {"fechada", "cerrada"}, {"excluído", "eliminado"}, {"excluída", "eliminada"}, {"vinculado", "vinculado"},
            {"vinculados", "vinculados"}, {"reordenar", "reordenar"}, {"ordenação", "ordenación"}, {"posição", "posición"},
            {"Operador de canal: quem cria o canal gerencia", "Operador del canal: quien lo crea lo administra"},
            {"membros temporários de canais seguem o grupo global", "los miembros de canales temporales siguen el grupo global"}
        };
        const QList<QPair<QString, QString>> enMissing = {
            {"Halla", "Halla"},
            {"comandante", "commander"},
            {"lista", "list"},
            {"offline", "offline"},
            {"sussurro", "whisper"},
            {"tecla", "key"},
            {"Apelido", "Nickname"},
            {"Definir", "Set"},
            {"apelido", "nickname"},
            {"Alternar", "Toggle"},
            {"avatar", "avatar"},
            {"Avatar", "Avatar"},
            {"favoritos", "bookmarks"},
            {"único", "unique"},
            {"Mensagens", "Messages"},
            {"Digite", "Enter"},
            {"Usar", "Use"},
            {"privilégio", "privilege"},
            {"Lista", "List"},
            {"Listas", "Lists"},
            {"Nova", "New"},
            {"possível", "possible"},
            {"bandeja", "tray"},
            {"mouse", "mouse"},
            {"Renomear", "Rename"},
            {"teste", "test"},
            {"Chave", "Key"},
            {"Iniciar", "Start"},
            {"Identidades", "Identities"},
            {"agora", "now"},
            {"contínua", "continuous"},
            {"Expulsar", "Kick"},
            {"atual", "current"},
            {"ícones", "icons"},
            {"contato", "contact"},
            {"Criar", "Create"},
            {"todos", "all"},
            {"conexão", "connection"},
            {"Gerenciar", "Manage"},
            {"como", "as"},
            {"alto-falantes", "speakers"},
            {"Transferência", "Transfer"},
            {"entrou", "joined"},
            {"atualização", "update"},
            {"Tecla", "Key"},
            {"banimento", "ban"},
            {"Data", "Date"},
            {"Limpar", "Clear"},
            {"deste", "this"},
            {"Ícone", "Icon"},
            {"identidade", "identity"},
            {"Perfis", "Profiles"},
            {"banido", "banned"},
            {"nova", "new"},
            {"Sair", "Exit"},
            {"Alterar", "Change"},
            {"chave", "key"},
            {"global", "global"},
            {"Registro", "Log"},
            {"Contatos", "Contacts"},
            {"Sobre", "About"},
            {"Verificar", "Check"},
            {"nenhum", "none"},
            {"versão", "version"},
            {"Deseja", "Do you want"},
            {"concluído", "complete"},
            {"Ping", "Ping"},
            {"pacotes", "packets"},
            {"Transmissão", "Transmission"},
            {"registrada", "registered"},
            {"Reclamação", "Complaint"},
            {"Cargo", "Role"},
            {"Imagens", "Images"},
            {"fala", "speech"},
            {"codec", "codec"},
            {"Conceder", "Grant"},
            {"Identidade", "Identity"},
            {"Arquivos", "Files"},
            {"Conexão", "Connection"},
            {"Versão", "Version"},
            {"eventos", "events"},
            {"Árvore", "Tree"},
            {"Expandir", "Expand"},
            {"receber", "receive"},
            {"Reproduzir", "Play"},
            {"Windows", "Windows"},
            {"Outros", "Other"},
            {"Mono", "Mono"},
            {"sinal", "cue"},
            {"Pesquisar", "Search"},
            {"locais", "local"},
            {"local", "local"},
            {"localmente", "locally"},
            {"expulso", "kicked"},
            {"reclamação", "complaint"},
            {"sobre", "about"},
            {"virtual", "virtual"},
            {"desativado", "disabled"},
            {"ativado", "enabled"},
            {"marcadores", "bookmarks"},
            {"mesmo", "same"},
            {"usada", "used"},
            {"apenas", "only"},
            {"comunicação", "communication"},
            {"favorito", "favorite"},
            {"baixar", "download"},
            {"usando", "using"},
            {"recente", "recent"},
            {"Baixando", "Downloading"},
            {"download", "download"},
            {"Parar", "Stop"},
            {"Perda", "Loss"},
            {"conexões", "connections"},
            {"transmissão", "transmission"},
            {"continua", "continues"},
            {"execução", "running"},
            {"realmente", "really"},
            {"sair", "exit"},
            {"ativos", "active"},
            {"selecionado", "selected"},
            {"edite", "edit"},
            {"Selecione", "Select"},
            {"Atribuir", "Assign"},
            {"interno", "built-in"},
            {"internos", "built-in"},
            {"podem", "can"},
            {"destinatários", "recipients"},
            {"Aceita", "Accepts"},
            {"Classificar", "Sort"},
            {"abaixo", "below"},
            {"precisa", "needs"},
            {"texto", "text"},
            {"Mais", "More"},
            {"fonético", "phonetic"},
            {"Filtro", "Filter"},
            {"suas", "your"},
            {"Cutucar", "Poke"},
            {"Banir", "Ban"},
            {"Teclas", "Keys"},
            {"sessão", "session"},
            {"Tamanho", "Size"},
            {"fazer", "do"},
            {"login", "login"},
            {"próprio", "own"},
            {"menus", "menus"},
            {"contexto", "context"},
            {"reproduzem", "play"},
            {"Quando", "When"},
            {"cutucado", "poked"},
            {"melhor", "best"},
            {"Emitir", "Play"},
            {"sincronizados", "synchronized"},
            {"requer", "requires"},
            {"nenhuma", "none"},
            {"atribuída", "assigned"},
            {"Chat", "Chat"},
            {"Clique", "Click"},
            {"aqui", "here"},
            {"mudos", "muted"},
            {"Registrar", "Register"},
            {"excluir", "delete"},
            {"exigirem", "require"},
            {"iniciada", "started"},
            {"segue", "follows"},
            {"selecionados", "selected"},
            {"recentes", "recent"},
            {"Operador", "Operator"},
            {"quem", "who"},
            {"cria", "creates"},
            {"gerencia", "manages"},
            {"membros", "members"},
            {"temporários", "temporary"},
            {"seguem", "follow"},
            {"Conecte-se", "Connect"},
            {"clique", "click"},
            {"iniciado", "started"},
            {"instalar", "install"},
            {"será", "will be"},
            {"executado", "run"},
            {"salvar", "save"},
            {"desativada", "disabled"},
            {"ainda", "still"},
            {"encerrado", "closed"},
            {"registrado", "registered"},
            {"registrar", "register"},
            {"Compilado", "Compiled"},
            {"direitos", "rights"},
            {"reservados", "reserved"},
            {"Banimentos", "Bans"},
            {"Expira", "Expires"},
            {"registradas", "registered"},
            {"pelos", "by"},
            {"TODAS", "ALL"},
            {"Crie", "Create"},
            {"atribua", "assign"},
            {"Emoji", "Emoji"},
            {"coroa", "crown"},
            {"Prefixo", "Prefix"},
            {"Sigla", "abbreviation"},
            {"Ordem", "Order"},
            {"Hierarquia", "Hierarchy"},
            {"Envio", "Upload"},
            {"tamanho", "size"},
            {"excluídos", "excluded"},
            {"voltarão", "will return"},
            {"renomeados", "renamed"},
            {"atribuído", "assigned"},
            {"Suas", "Your"},
            {"talkPower", "talk power"},
            {"deixadas", "left"},
            {"enquanto", "while"},
            {"estava", "was"},
            {"outros", "others"},
            {"Destinatário", "Recipient"},
            {"entregue", "delivered"},
            {"Será", "Will be"},
            {"mostrada", "shown"},
            {"Adicione", "Add"},
            {"conecte-se", "connect"},
            {"seus", "your"},
            {"Auto", "Automatic"},
            {"Rótulo", "Label"},
            {"Porta", "Port"},
            {"iniciar", "start"},
            {"propriedades", "properties"},
            {"branco", "white"},
            {"kbps", "kbps"},
            {"Bitrate", "Bitrate"},
            {"ordenado", "sorted"},
            {"símbolo", "symbol"},
            {"Menos", "Less"},
            {"Atribua", "Assign"},
            {"atualizados", "updated"},
            {"Gerencie", "Manage"},
            {"identidades", "identities"},
            {"chaves", "keys"},
            {"exclusivas", "exclusive"},
            {"identifica", "identifies"},
            {"perante", "before"},
            {"manter", "keep"},
            {"Eventos", "Events"},
            {"salvos", "saved"},
            {"Tudo", "Everything"},
            {"Info", "Info"},
            {"Aviso", "Warning"},
            {"Depuração", "Debug"},
            {"Rolagem", "Scrolling"},
            {"recebida", "received"},
            {"administrador", "administrator"},
            {"cutucada", "poke"},
            {"opcional", "optional"},
            {"Plataforma", "Platform"},
            {"Conectados", "Connected"},
            {"Uptime", "Uptime"},
            {"gerais", "general"},
            {"aplicativo", "application"},
            {"aparência", "appearance"},
            {"avisos", "alerts"},
            {"segurança", "security"},
            {"Extensões", "Extensions"},
            {"aplicadas", "applied"},
            {"Inicialização", "Startup"},
            {"Restaurar", "Restore"},
            {"anterior", "previous"},
            {"avançado", "advanced"},
            {"Janela", "Window"},
            {"Confirmar", "Confirm"},
            {"estando", "being"},
            {"nativo", "native"},
            {"Fusion", "Fusion"},
            {"Estilo", "Style"},
            {"Tema", "Theme"},
            {"Pacote", "Package"},
            {"Obter", "Get"},
            {"folhas", "sheets"},
            {"estilos", "styles"},
            {"Fonte", "Font"},
            {"fonte", "font"},
            {"Transparência", "Opacity"},
            {"até", "up to"},
            {"bandeira", "flag"},
            {"país", "country"},
            {"Overwolf", "Overwolf"},
            {"emblema", "badge"},
            {"inacessíveis", "inaccessible"},
            {"número", "number"},
            {"mini-ícones", "mini-icons"},
            {"ausência", "away"},
            {"dica", "tooltip"},
            {"passar", "hover"},
            {"Minimizar", "Minimize"},
            {"animados", "animated"},
            {"animadas", "animated"},
            {"Escolha", "Choose"},
            {"quais", "which"},
            {"entra", "enters"},
            {"Narrar", "Narrate"},
            {"texto-para-voz", "text-to-speech"},
            {"pacote", "package"},
            {"Nivelamento", "Leveling"},
            {"automático", "automatic"},
            {"reproduz", "plays"},
            {"comfort", "comfort"},
            {"noise", "noise"},
            {"Ruído", "Noise"},
            {"Expansão", "Expansion"},
            {"estéreo", "stereo"},
            {"alto-falante", "speaker"},
            {"surround", "surround"},
            {"Pressionar", "Press"},
            {"Atraso", "Delay"},
            {"atividade", "activity"},
            {"Sensível", "Sensitive"},
            {"Restrito", "Restricted"},
            {"Processamento", "Processing"},
            {"digital", "digital"},
            {"ruídos", "noises"},
            {"Cancelamento", "Cancellation"},
            {"Redução", "Reduction"},
            {"Ducking", "Ducking"},
            {"sonoro", "sound"},
            {"computador", "computer"},
            {"Também", "Also"},
            {"emitir", "emit"},
            {"outro", "other"},
            {"flac", "flac"},
            {"sinais", "cues"}
        };
        const QList<QPair<QString, QString>> esMissing = {
            {"Halla", "Halla"},
            {"comandante", "comandante"},
            {"lista", "lista"},
            {"offline", "sin conexión"},
            {"sussurro", "susurro"},
            {"tecla", "tecla"},
            {"Apelido", "Apodo"},
            {"Definir", "Definir"},
            {"apelido", "apodo"},
            {"Alternar", "Alternar"},
            {"avatar", "avatar"},
            {"Avatar", "Avatar"},
            {"favoritos", "favoritos"},
            {"único", "único"},
            {"Mensagens", "Mensajes"},
            {"Digite", "Escribe"},
            {"Usar", "Usar"},
            {"privilégio", "privilegio"},
            {"Lista", "Lista"},
            {"Listas", "Listas"},
            {"Nova", "Nueva"},
            {"possível", "posible"},
            {"bandeja", "bandeja"},
            {"mouse", "ratón"},
            {"Renomear", "Renombrar"},
            {"teste", "prueba"},
            {"Chave", "Clave"},
            {"Iniciar", "Iniciar"},
            {"Identidades", "Identidades"},
            {"agora", "ahora"},
            {"contínua", "continua"},
            {"Expulsar", "Expulsar"},
            {"atual", "actual"},
            {"ícones", "iconos"},
            {"contato", "contacto"},
            {"Criar", "Crear"},
            {"todos", "todos"},
            {"conexão", "conexión"},
            {"Gerenciar", "Administrar"},
            {"como", "como"},
            {"alto-falantes", "altavoces"},
            {"Transferência", "Transferencia"},
            {"entrou", "entró"},
            {"atualização", "actualización"},
            {"Tecla", "Tecla"},
            {"banimento", "baneo"},
            {"Data", "Fecha"},
            {"Limpar", "Limpiar"},
            {"deste", "de este"},
            {"Ícone", "Icono"},
            {"identidade", "identidad"},
            {"Perfis", "Perfiles"},
            {"banido", "baneado"},
            {"nova", "nueva"},
            {"Sair", "Salir"},
            {"Alterar", "Cambiar"},
            {"chave", "clave"},
            {"global", "global"},
            {"Registro", "Registro"},
            {"Contatos", "Contactos"},
            {"Sobre", "Acerca de"},
            {"Verificar", "Buscar"},
            {"nenhum", "ninguno"},
            {"versão", "versión"},
            {"Deseja", "¿Deseas"},
            {"concluído", "completado"},
            {"Ping", "Ping"},
            {"pacotes", "paquetes"},
            {"Transmissão", "Transmisión"},
            {"registrada", "registrada"},
            {"Reclamação", "Queja"},
            {"Cargo", "Cargo"},
            {"Imagens", "Imágenes"},
            {"fala", "habla"},
            {"codec", "códec"},
            {"Conceder", "Conceder"},
            {"Identidade", "Identidad"},
            {"Arquivos", "Archivos"},
            {"Conexão", "Conexión"},
            {"Versão", "Versión"},
            {"eventos", "eventos"},
            {"Árvore", "Árbol"},
            {"Expandir", "Expandir"},
            {"receber", "recibir"},
            {"Reproduzir", "Reproducir"},
            {"Windows", "Windows"},
            {"Outros", "Otros"},
            {"Mono", "Mono"},
            {"sinal", "señal"},
            {"Pesquisar", "Buscar"},
            {"locais", "locales"},
            {"local", "local"},
            {"localmente", "localmente"},
            {"expulso", "expulsado"},
            {"reclamação", "queja"},
            {"sobre", "sobre"},
            {"virtual", "virtual"},
            {"desativado", "desactivado"},
            {"ativado", "activado"},
            {"marcadores", "marcadores"},
            {"mesmo", "mismo"},
            {"usada", "usada"},
            {"apenas", "solo"},
            {"comunicação", "comunicación"},
            {"favorito", "favorito"},
            {"baixar", "descargar"},
            {"usando", "usando"},
            {"recente", "reciente"},
            {"Baixando", "Descargando"},
            {"download", "descarga"},
            {"Parar", "Detener"},
            {"Perda", "Pérdida"},
            {"conexões", "conexiones"},
            {"transmissão", "transmisión"},
            {"continua", "continúa"},
            {"execução", "ejecución"},
            {"realmente", "realmente"},
            {"sair", "salir"},
            {"ativos", "activos"},
            {"selecionado", "seleccionado"},
            {"edite", "edita"},
            {"Selecione", "Selecciona"},
            {"Atribuir", "Asignar"},
            {"interno", "interno"},
            {"internos", "internos"},
            {"podem", "pueden"},
            {"destinatários", "destinatarios"},
            {"Aceita", "Acepta"},
            {"Classificar", "Ordenar"},
            {"abaixo", "debajo"},
            {"precisa", "necesita"},
            {"texto", "texto"},
            {"Mais", "Más"},
            {"fonético", "fonético"},
            {"Filtro", "Filtro"},
            {"suas", "tus"},
            {"Cutucar", "Dar un toque"},
            {"Banir", "Banear"},
            {"Teclas", "Teclas"},
            {"sessão", "sesión"},
            {"Tamanho", "Tamaño"},
            {"fazer", "hacer"},
            {"login", "inicio de sesión"},
            {"próprio", "propio"},
            {"menus", "menús"},
            {"contexto", "contexto"},
            {"reproduzem", "reproducen"},
            {"Quando", "Cuando"},
            {"cutucado", "molestado"},
            {"melhor", "mejor"},
            {"Emitir", "Emitir"},
            {"sincronizados", "sincronizados"},
            {"requer", "requiere"},
            {"nenhuma", "ninguna"},
            {"atribuída", "asignada"},
            {"Chat", "Chat"},
            {"Clique", "Haz clic"},
            {"aqui", "aquí"},
            {"mudos", "silenciados"},
            {"Registrar", "Registrar"},
            {"excluir", "eliminar"},
            {"exigirem", "requieren"},
            {"iniciada", "iniciada"},
            {"segue", "sigue"},
            {"selecionados", "seleccionados"},
            {"recentes", "recientes"},
            {"Operador", "Operador"},
            {"quem", "quien"},
            {"cria", "crea"},
            {"gerencia", "administra"},
            {"membros", "miembros"},
            {"temporários", "temporales"},
            {"seguem", "siguen"},
            {"Conecte-se", "Conéctate"},
            {"clique", "clic"},
            {"iniciado", "iniciado"},
            {"instalar", "instalar"},
            {"será", "será"},
            {"executado", "ejecutará"},
            {"salvar", "guardar"},
            {"desativada", "desactivada"},
            {"ainda", "todavía"},
            {"encerrado", "cerrado"},
            {"registrado", "registrado"},
            {"registrar", "registrar"},
            {"Compilado", "Compilado"},
            {"direitos", "derechos"},
            {"reservados", "reservados"},
            {"Banimentos", "Baneos"},
            {"Expira", "Expira"},
            {"registradas", "registradas"},
            {"pelos", "por los"},
            {"TODAS", "TODAS"},
            {"Crie", "Crea"},
            {"atribua", "asigna"},
            {"Emoji", "Emoji"},
            {"coroa", "corona"},
            {"Prefixo", "Prefijo"},
            {"Sigla", "Abreviatura"},
            {"Ordem", "Orden"},
            {"Hierarquia", "Jerarquía"},
            {"Envio", "Envío"},
            {"tamanho", "tamaño"},
            {"excluídos", "eliminados"},
            {"voltarão", "volverán"},
            {"renomeados", "renombrados"},
            {"atribuído", "asignado"},
            {"Suas", "Tus"},
            {"talkPower", "poder de habla"},
            {"deixadas", "dejadas"},
            {"enquanto", "mientras"},
            {"estava", "estaba"},
            {"outros", "otros"},
            {"Destinatário", "Destinatario"},
            {"entregue", "entregado"},
            {"Será", "Será"},
            {"mostrada", "mostrada"},
            {"Adicione", "Añade"},
            {"conecte-se", "conéctate"},
            {"seus", "tus"},
            {"Auto", "Automático"},
            {"Rótulo", "Etiqueta"},
            {"Porta", "Puerto"},
            {"iniciar", "iniciar"},
            {"propriedades", "propiedades"},
            {"branco", "blanco"},
            {"kbps", "kbps"},
            {"Bitrate", "Bitrate"},
            {"ordenado", "ordenado"},
            {"símbolo", "símbolo"},
            {"Menos", "Menos"},
            {"Atribua", "Asigna"},
            {"atualizados", "actualizados"},
            {"Gerencie", "Administra"},
            {"identidades", "identidades"},
            {"chaves", "claves"},
            {"exclusivas", "exclusivas"},
            {"identifica", "identifica"},
            {"perante", "ante"},
            {"manter", "mantener"},
            {"Eventos", "Eventos"},
            {"salvos", "guardados"},
            {"Tudo", "Todo"},
            {"Info", "Info"},
            {"Aviso", "Aviso"},
            {"Depuração", "Depuración"},
            {"Rolagem", "Desplazamiento"},
            {"recebida", "recibida"},
            {"administrador", "administrador"},
            {"cutucada", "toque"},
            {"opcional", "opcional"},
            {"Plataforma", "Plataforma"},
            {"Conectados", "Conectados"},
            {"Uptime", "Tiempo activo"},
            {"gerais", "generales"},
            {"aplicativo", "aplicación"},
            {"aparência", "apariencia"},
            {"avisos", "avisos"},
            {"segurança", "seguridad"},
            {"Extensões", "Extensiones"},
            {"aplicadas", "aplicadas"},
            {"Inicialização", "Inicio"},
            {"Restaurar", "Restaurar"},
            {"anterior", "anterior"},
            {"avançado", "avanzado"},
            {"Janela", "Ventana"},
            {"Confirmar", "Confirmar"},
            {"estando", "estando"},
            {"nativo", "nativo"},
            {"Fusion", "Fusion"},
            {"Estilo", "Estilo"},
            {"Tema", "Tema"},
            {"Pacote", "Paquete"},
            {"Obter", "Obtener"},
            {"folhas", "hojas"},
            {"estilos", "estilos"},
            {"Fonte", "Fuente"},
            {"fonte", "fuente"},
            {"Transparência", "Transparencia"},
            {"até", "hasta"},
            {"bandeira", "bandera"},
            {"país", "país"},
            {"Overwolf", "Overwolf"},
            {"emblema", "insignia"},
            {"inacessíveis", "inaccesibles"},
            {"número", "número"},
            {"mini-ícones", "mini-iconos"},
            {"ausência", "ausencia"},
            {"dica", "consejo"},
            {"passar", "pasar"},
            {"Minimizar", "Minimizar"},
            {"animados", "animados"},
            {"animadas", "animadas"},
            {"Escolha", "Elige"},
            {"quais", "cuáles"},
            {"entra", "entra"},
            {"Narrar", "Narrar"},
            {"texto-para-voz", "texto a voz"},
            {"pacote", "paquete"},
            {"Nivelamento", "Nivelación"},
            {"automático", "automático"},
            {"reproduz", "reproduce"},
            {"comfort", "comfort"},
            {"noise", "noise"},
            {"Ruído", "Ruido"},
            {"Expansão", "Expansión"},
            {"estéreo", "estéreo"},
            {"alto-falante", "altavoz"},
            {"surround", "surround"},
            {"Pressionar", "Presionar"},
            {"Atraso", "Retraso"},
            {"atividade", "actividad"},
            {"Sensível", "Sensible"},
            {"Restrito", "Restringido"},
            {"Processamento", "Procesamiento"},
            {"digital", "digital"},
            {"ruídos", "ruidos"},
            {"Cancelamento", "Cancelación"},
            {"Redução", "Reducción"},
            {"Ducking", "Ducking"},
            {"sonoro", "sonora"},
            {"computador", "ordenador"},
            {"Também", "También"},
            {"emitir", "emitir"},
            {"outro", "otro"},
            {"flac", "flac"},
            {"sinais", "señales"}
        };
        for (const auto& pair : en) m_enWords.insert(pair.first, pair.second);
        for (const auto& pair : es) m_esWords.insert(pair.first, pair.second);
        for (const auto& pair : enExtra) m_enWords.insert(pair.first, pair.second);
        for (const auto& pair : esExtra) m_esWords.insert(pair.first, pair.second);
        for (const auto& pair : enMissing) m_enWords.insert(pair.first, pair.second);
        for (const auto& pair : esMissing) m_esWords.insert(pair.first, pair.second);
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
        m_en["Mostrar descrição e informações do servidor"] = "Show server description and information";
        m_en["Cargos:"] = "Roles:";
        m_en["Usuários neste grupo"] = "Users in this group";
        m_en["Nome"] = "Name";
        m_en["UID"] = "UID";
        m_en["Estado"] = "Status";
        m_en["Remover do grupo"] = "Remove from group";
        m_en["online"] = "online";
        m_en["offline"] = "offline";

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
        m_es["Mostrar descrição e informações do servidor"] = "Mostrar descripción e información del servidor";
        m_es["Cargos:"] = "Cargos:";
        m_es["Usuários neste grupo"] = "Usuarios en este grupo";
        m_es["Nome"] = "Nombre";
        m_es["UID"] = "UID";
        m_es["Estado"] = "Estado";
        m_es["Remover do grupo"] = "Quitar del grupo";
        m_es["online"] = "en línea";
        m_es["offline"] = "sin conexión";

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
