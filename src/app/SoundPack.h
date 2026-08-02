#pragma once

#include <QString>
#include <QStringList>

// Pacote de sons do Halla (estilo TeamSpeak 3):
// na primeira execução, gera pequenos arquivos WAV em <config>/sounds e os
// reproduz via QSoundEffect nos eventos de notificação.
namespace HSound {

// eventos disponíveis (nomes de arquivo sem extensão)
inline QStringList names() {
    return { QStringLiteral("connected"),     // conectou ao servidor
             QStringLiteral("disconnected"),  // desconectou
             QStringLiteral("user_joined"),   // cliente entrou no servidor/canal
             QStringLiteral("user_left"),     // cliente saiu
             QStringLiteral("message"),       // mensagem privada/canal
             QStringLiteral("poke"),          // cutucada
             QStringLiteral("mic_muted"),     // microfone mudo
             QStringLiteral("mic_unmuted"),   // microfone ativo
             QStringLiteral("recording"),     // gravação iniciada
             QStringLiteral("test") };        // som de teste das opções
}

void ensure();                  // gera os WAVs padrão se ainda não existirem
void play(const QString& name); // toca <config>/sounds/<name>.wav (não bloqueia)
QString dir();                  // pasta dos sons

}
