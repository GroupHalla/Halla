#pragma once

#include <QString>
#include <QStringList>

// Halla sound pack (Halla style): on first run it generates small WAV files under
// <config>/sounds and plays them through QSoundEffect on notification events.
namespace HSound {

// available events (file names without extension)
inline QStringList names() {
    return { QStringLiteral("connected"),               // conectou ao servidor
             QStringLiteral("connection_lost"),         // connection dropped unexpectedly
             QStringLiteral("disconnected"),            // manual disconnect
             QStringLiteral("moved"),                   // you were moved / switched channel
             QStringLiteral("user_joined"),             // another client joined your channel
             QStringLiteral("user_left"),               // a client left your channel
             QStringLiteral("kicked"),                  // kicked from the server
             QStringLiteral("banned"),                  // banned from the server
             QStringLiteral("message"),                 // mensagem privada/canal
             QStringLiteral("poke"),                    // cutucada / wake up
             QStringLiteral("error"),                   // erro geral
             QStringLiteral("insufficient_permissions"), // insufficient permission
             QStringLiteral("mic_muted"),               // microfone mudo
             QStringLiteral("mic_unmuted"),             // microfone ativo
             QStringLiteral("sound_muted"),             // playback muted
             QStringLiteral("sound_resumed"),           // playback unmuted
             QStringLiteral("recording"),               // recording started
             QStringLiteral("test") };                  // options test sound
}

void ensure();                  // generates the default WAVs if they do not exist yet
void play(const QString& name); // plays <config>/sounds/<name>.wav (non-blocking)
void playFile(const QString& path); // plays a user-chosen file
QString dir();                  // pasta dos sons

}
