#pragma once

#include <QString>
#include <QStringList>

// Pacote de sons do Halla (estilo Halla):
// na primeira execução, gera pequenos arquivos WAV em <config>/sounds e os
// reproduz via QSoundEffect nos eventos de notificação.
namespace HSound {

// eventos disponíveis (nomes de arquivo sem extensão)
inline QStringList names() {
    return { QStringLiteral("connected"),               // conectou ao servidor
             QStringLiteral("connection_lost"),         // conexão caiu inesperadamente
             QStringLiteral("disconnected"),            // desconexão manual
             QStringLiteral("moved"),                   // você foi movido/trocou de canal
             QStringLiteral("user_joined"),             // outro cliente entrou no seu canal
             QStringLiteral("user_left"),               // cliente saiu do seu canal
             QStringLiteral("kicked"),                  // expulso do servidor
             QStringLiteral("banned"),                  // banido do servidor
             QStringLiteral("message"),                 // mensagem privada/canal
             QStringLiteral("poke"),                    // cutucada / wake up
             QStringLiteral("error"),                   // erro geral
             QStringLiteral("insufficient_permissions"), // permissão insuficiente
             QStringLiteral("mic_muted"),               // microfone mudo
             QStringLiteral("mic_unmuted"),             // microfone ativo
             QStringLiteral("sound_muted"),             // reprodução silenciada
             QStringLiteral("sound_resumed"),           // reprodução reativada
             QStringLiteral("recording"),               // gravação iniciada
             QStringLiteral("test") };                  // som de teste das opções
}

void ensure();                  // gera os WAVs padrão se ainda não existirem
void play(const QString& name); // toca <config>/sounds/<name>.wav (não bloqueia)
void playFile(const QString& path); // toca um arquivo escolhido pelo usuário
QString dir();                  // pasta dos sons

}
