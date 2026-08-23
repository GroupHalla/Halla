#pragma once

#include <QString>

// Texto-para-voz do Halla (estilo "Default Sound Pack" com voz do Halla):
// narra eventos ("Fulano entrou", "Você foi cutucado") quando ativado em
// Opções > Notificações. Usa QTextToSpeech (Windows: SAPI; Linux: speechd).
namespace HSpeech {

void say(const QString& text); // assíncrono; ignora se desativado/sem engine
bool available();              // existe engine de voz no sistema?

}
