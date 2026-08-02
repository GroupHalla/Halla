#include "Speech.h"
#include "Settings.h"

#include <QTextToSpeech>
#include <QLocale>

namespace HSpeech {

static QTextToSpeech* engine() {
    static QTextToSpeech* tts = [] {
        QTextToSpeech* t = new QTextToSpeech;
        t->setRate(0.0);
        t->setVolume(0.85);
        // prefere voz em português quando houver
        const QLocale pt(QLocale::Portuguese, QLocale::Brazil);
        for (const QLocale& l : t->availableLocales())
            if (l.language() == QLocale::Portuguese) { t->setLocale(l); break; }
        return t;
    }();
    return tts;
}

bool available() {
    QTextToSpeech* t = engine();
    return t && t->state() != QTextToSpeech::Error &&
           !t->availableVoices().isEmpty();
}

void say(const QString& text) {
    if (!S::flag("notify/ttsEnabled", false)) return;
    QTextToSpeech* t = engine();
    if (!t || t->state() == QTextToSpeech::Error) return;
    t->say(text);
}

} // namespace HSpeech
