#pragma once

#include <QString>

// Halla text-to-speech (Halla-voice style of the "Default Sound Pack"):
// narrates events ("So-and-so joined", "You were poked") when enabled in
// Options > Notifications. Uses QTextToSpeech (Windows: SAPI; Linux: speechd).
namespace HSpeech {

void say(const QString& text); // asynchronous; ignored when disabled or without an engine
bool available();              // is there a voice engine on the system?

}
