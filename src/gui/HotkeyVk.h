#pragma once

// ============================================================================
// HotkeyVk — núcleo PORTÁTIL do mapeamento "spec de atalho" <-> teclas.
//
// Não depende de Qt nem de windows.h: os bits de modificador e os códigos de
// tecla usados são os MESMOS valores estáveis do Qt (QKeyCombination::
// toCombined()), e os VK/MOD_* do Windows entram como constantes literais.
// Isso permite compilar o mapeamento em qualquer plataforma — em particular
// nos smoke tests do CI (g++/cl sem Qt) e nos testes locais do Linux.
//
// POR QUE ISTO EXISTE (v1.1.27): o conversor antigo (MainWindow::
// specToVkImpl) só conhecia letras, dígitos, F1–F24 e teclas de controle.
// Toda tecla de pontuação/OEM ("\", ";", "'", "[", "]", ",", "/", "=", "`",
// "ç", acentos...) caía fora da tabela: vk ficava 0, a função devolvia false
// e o chamador DESCARTAVA a tecla em silêncio. A captura funcionava (o campo
// mostrava a tecla), mas no runtime nada era registrado — PTT, sussurro e
// atalhos genéricos ficavam inertes, "como se o usuário não apertasse nada".
//
// As teclas de pontuação são justamente as mais usadas como tecla de
// sussurro/PTT em layouts não-US (ABNT2 no Brasil: "\" é tecla direta, "ç"
// existe, "/" fica na tecla extra ao lado do Shift direito).
//
// O formato da spec é o mesmo que o QKeySequence::toString() sempre gerou
// ("Ctrl+F2", "Space", "A", "\\"), ESTENDIDO com os modificadores que o
// toString pode omitir: "Meta+" (tecla Windows) e "Num+" (teclado numérico).
// ============================================================================

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace hvk {

// ---- bits de modificador (valores estáveis do Qt::KeyboardModifier) -------
constexpr int kShiftMod  = 0x02000000;
constexpr int kCtrlMod   = 0x04000000;
constexpr int kAltMod    = 0x08000000;
constexpr int kMetaMod   = 0x10000000; // tecla Windows/Command
constexpr int kKeypadMod = 0x20000000; // teclado numérico
constexpr int kModMask   = 0xFE000000;

// ---- códigos de tecla de controle (valores estáveis do Qt::Key) -----------
constexpr int kKeyTab        = 0x01000001;
constexpr int kKeyBackspace  = 0x01000003;
constexpr int kKeyReturn     = 0x01000004;
constexpr int kKeyEnter      = 0x01000005;
constexpr int kKeyInsert     = 0x01000006;
constexpr int kKeyDelete     = 0x01000007;
constexpr int kKeyPause      = 0x01000008;
constexpr int kKeyPrint      = 0x01000009;
constexpr int kKeyHome       = 0x01000010;
constexpr int kKeyEnd        = 0x01000011;
constexpr int kKeyLeft       = 0x01000012;
constexpr int kKeyUp         = 0x01000013;
constexpr int kKeyRight      = 0x01000014;
constexpr int kKeyDown       = 0x01000015;
constexpr int kKeyPageUp     = 0x01000016;
constexpr int kKeyPageDown   = 0x01000017;
constexpr int kKeyCapsLock   = 0x01000024;
constexpr int kKeyNumLock    = 0x01000025;
constexpr int kKeyScrollLock = 0x01000026;
constexpr int kKeyFBase      = 0x01000030; // F1; F24 = kKeyFBase + 23

// ---- MOD_* do Windows (valores literais do winuser.h) ---------------------
constexpr unsigned kModAlt     = 0x0001;
constexpr unsigned kModControl = 0x0002;
constexpr unsigned kModShift   = 0x0004;
constexpr unsigned kModWin     = 0x0008;

// ---- VKs relevantes (valores literais) -------------------------------------
constexpr unsigned kVkNumPad0  = 0x60; // .. kVkNumPad0 + 9
constexpr unsigned kVkMultiply = 0x6A;
constexpr unsigned kVkAdd      = 0x6B;
constexpr unsigned kVkSubtract = 0x6D;
constexpr unsigned kVkDecimal  = 0x6E;
constexpr unsigned kVkDivide   = 0x6F;

// Resolvedor layout-dependente: recebe um caractere (Unicode <= 0xFF) e
// devolve o VK no byte baixo, com os bits 0x100/0x200/0x400 marcando
// shift/ctrl/alt exigidos pelo layout ATUAL para produzir o caractere —
// ou -1 se o caractere não existir no layout. No Windows a implementação
// é VkKeyScanW(); nos testes é um mock (layout US, ABNT2, "vazio"...).
using VkFromCharFn = int (*)(unsigned ch);

// ---------------------------------------------------------------------------
// Fallback US para pontuação de digitação direta (sem shift). Só é usado
// quando o resolvedor do layout falha ou não existe — cobre o caso de o
// layout ativo não responder pelo caractere sem deixá-lo morto.
inline unsigned fallbackUsVk(int key) {
    switch (key) {
    case ' ':  return 0x20; // VK_SPACE
    case ';':  return 0xBA; // VK_OEM_1
    case '=':  return 0xBB; // VK_OEM_PLUS
    case ',':  return 0xBC; // VK_OEM_COMMA
    case '-':  return 0xBD; // VK_OEM_MINUS
    case '.':  return 0xBE; // VK_OEM_PERIOD
    case '/':  return 0xBF; // VK_OEM_2
    case '`':  return 0xC0; // VK_OEM_3
    case '[':  return 0xDB; // VK_OEM_4
    case '\\': return 0xDC; // VK_OEM_5
    case ']':  return 0xDD; // VK_OEM_6
    case '\'': return 0xDE; // VK_OEM_7
    default:   return 0;
    }
}

// ---------------------------------------------------------------------------
// Converte o combined (mods Qt | key, layout do QKeyCombination::toCombined)
// em VK + MOD_* do Windows.
//
// Cobertura:
//  - tudo o que o conversor antigo já mapeava (regressão zero);
//  - teclas de pontuação/acento Latin-1 imprimíveis (0x20..0xFF): resolvidas
//    NO LAYOUT ATIVO via vkFromChar (VkKeyScanW) — "\\", "ç", ";", "`"...
//    caem na tecla física certa em qualquer layout — com fallback US;
//  - teclado numérico ("Num+5" -> VK_NUMPAD5, "Num++" -> VK_ADD...);
//  - modificador Meta/Windows -> MOD_WIN.
//
// `mods` sai LIMPO (só MOD_ALT/CONTROL/SHIFT/WIN reais, sem MOD_NOREPEAT —
// quem registra hotkey adiciona o que precisa).
inline bool specToVkCore(int combined, VkFromCharFn vkFromChar,
                         unsigned& vk, unsigned& mods) {
    vk = 0;
    mods = 0;
    const int qtMods = combined & kModMask;
    const int key = combined & ~kModMask;
    if (!key) return false;
    if (qtMods & ~(kShiftMod | kCtrlMod | kAltMod | kMetaMod | kKeypadMod))
        return false; // modificador desconhecido

    const auto applyQtMods = [&] {
        if (qtMods & kShiftMod) mods |= kModShift;
        if (qtMods & kCtrlMod)  mods |= kModControl;
        if (qtMods & kAltMod)   mods |= kModAlt;
        if (qtMods & kMetaMod)  mods |= kModWin;
    };

    // teclado numérico: distinto da fileira principal (com NumLock, o VK
    // reportado é VK_NUMPAD0..9, não o dígito da fileira de cima)
    if (qtMods & kKeypadMod) {
        if (key >= '0' && key <= '9') { vk = kVkNumPad0 + unsigned(key - '0'); applyQtMods(); return true; }
        if (key == '+') { vk = kVkAdd;      applyQtMods(); return true; }
        if (key == '-') { vk = kVkSubtract; applyQtMods(); return true; }
        if (key == '*') { vk = kVkMultiply; applyQtMods(); return true; }
        if (key == '/') { vk = kVkDivide;   applyQtMods(); return true; }
        if (key == '.') { vk = kVkDecimal;  applyQtMods(); return true; }
        // "Num+Return"/"Num+Enter": numpad-enter reporta VK_RETURN comum —
        // segue como Return normal abaixo
    }

    // letras e dígitos ASCII (VK coincide com o código do caractere)
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
        vk = unsigned(key);
        applyQtMods();
        return true;
    }

    // F1..F24 (VK_F1..VK_F24 = 0x70..0x87)
    if (key >= kKeyFBase && key <= kKeyFBase + 23) {
        vk = 0x70 + unsigned(key - kKeyFBase);
        applyQtMods();
        return true;
    }

    switch (key) {
    case 0x20:           vk = 0x20; break; // Space -> VK_SPACE
    case kKeyTab:        vk = 0x09; break;
    case kKeyCapsLock:   vk = 0x14; break;
    case kKeyReturn:
    case kKeyEnter:      vk = 0x0D; break;
    case kKeyBackspace:  vk = 0x08; break;
    case kKeyInsert:     vk = 0x2D; break;
    case kKeyDelete:     vk = 0x2E; break;
    case kKeyHome:       vk = 0x24; break;
    case kKeyEnd:        vk = 0x23; break;
    case kKeyPageUp:     vk = 0x21; break;
    case kKeyPageDown:   vk = 0x22; break;
    case kKeyPrint:      vk = 0x2C; break;
    case kKeyPause:      vk = 0x13; break;
    case kKeyLeft:       vk = 0x25; break;
    case kKeyUp:         vk = 0x26; break;
    case kKeyRight:      vk = 0x27; break;
    case kKeyDown:       vk = 0x28; break;
    default: {
        // caractere imprimível Latin-1: pontuação, acentos, ç...
        if (key < 0x20 || key > 0xFF || key == 0x7F) return false;

        // 1) resolve NO LAYOUT ATIVO: devolve a tecla física certa (ABNT2,
        //    europeus...) e os mods que o layout exige (ex.: "+" pede shift
        //    no US; AltGr entra como ctrl+alt e casa com o polling físico)
        if (vkFromChar) {
            const int r = vkFromChar(unsigned(key));
            if (r >= 0) {
                vk = unsigned(r & 0xFF);
                if (r & 0x100) mods |= kModShift;
                if (r & 0x200) mods |= kModControl;
                if (r & 0x400) mods |= kModAlt;
            }
        }
        // 2) sem layout que responda: tabela US de digitação direta
        if (!vk) vk = fallbackUsVk(key);
        if (!vk) return false; // não existe no layout nem no fallback
        break;
    }
    }

    applyQtMods();
    return true;
}

// ---------------------------------------------------------------------------
// Parser manual da spec "Mods+Tecla" — pega tudo o que o QKeySequence::
// fromString() rejeita (chars de pontuação soltos, "Num+5", "ç"...).
// Devolve o combined (mods | key) ou 0 se não entender.
//
// Formato: tokens separados por "+"; os primeiros são modificadores
// (Ctrl/Control, Shift, Alt, Meta/Win/Super, Num/Keypad) e o último é a
// tecla: um caractere imprimível (ASCII ou Latin-1 em UTF-8) OU um nome
// (Space, Tab, Backspace, Return, Enter, Ins/Insert, Del/Delete, Pause,
// Print, Home, End, Left, Up, Right, Down, PgUp/PageUp, PgDown/PageDown,
// CapsLock, NumLock, ScrollLock, F1..F24). Um "+" final conta como a tecla
// "+" ("Ctrl++", "Num++").
inline int parseSpec(const char* utf8) {
    if (!utf8) return 0;
    std::string s(utf8);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (s.empty()) return 0;

    // separa "Mods+" (prefixo) da TECLA (sufixo). A tecla '+' é ambígua com
    // o separador: "+", "Ctrl++", "Num++" — termina em "++" (ou é só "+").
    std::string keyStr;
    std::string modsStr;
    if (s.back() == '+') {
        if (s.size() == 1) {                          // tecla '+' pura
            keyStr = "+";
        } else if (s[s.size() - 2] == '+') {          // "Ctrl++" / "Num++"
            keyStr = "+";
            modsStr = s.substr(0, s.size() - 2);
        }
        // termina em '+' sem "++": "Ctrl+" — tecla ausente, inválido
    } else {
        const size_t lastPlus = s.rfind('+');
        if (lastPlus == std::string::npos) keyStr = s;
        else {
            keyStr = s.substr(lastPlus + 1);
            modsStr = s.substr(0, lastPlus);
        }
    }
    if (keyStr.empty()) return 0;

    // modificadores: split do prefixo por '+'; um token vazio NO FIM é só o
    // separador pendente ("Ctrl+Num+" da tecla '+'); vazio no meio é erro
    int mods = 0;
    if (!modsStr.empty()) {
        size_t i = 0;
        while (i <= modsStr.size()) {
            const size_t j = modsStr.find('+', i);
            const std::string tok =
                (j == std::string::npos) ? modsStr.substr(i)
                                         : modsStr.substr(i, j - i);
            if (!tok.empty()) {
                std::string t = tok;
                for (char& c : t) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
                if      (t == "ctrl" || t == "control") mods |= kCtrlMod;
                else if (t == "shift")                  mods |= kShiftMod;
                else if (t == "alt")                    mods |= kAltMod;
                else if (t == "meta" || t == "win" || t == "super") mods |= kMetaMod;
                else if (t == "num" || t == "keypad")   mods |= kKeypadMod;
                else return 0; // modificador desconhecido
            } else if (j != std::string::npos) {
                return 0;      // token vazio no meio: "Ctrl++Alt+X" e afins
            }
            if (j == std::string::npos) break;
            i = j + 1;
        }
    }

    // a tecla: caractere imprimível ou nome
    int key = 0;
    if (keyStr.size() == 1) {
        const unsigned char c = (unsigned char)keyStr[0];
        if (c >= 0x21 && c <= 0x7E) key = int(c); // imprimível ASCII (não espaço)
    } else if (keyStr.size() == 2 &&
               (unsigned char)keyStr[0] >= 0xC2 && (unsigned char)keyStr[0] <= 0xC5) {
        // Latin-1 supplement em UTF-8 (ç, á, ã...): 110xxxxx 10xxxxxx
        const unsigned cp = ((unsigned char)keyStr[0] & 0x1Fu) << 6 |
                            ((unsigned char)keyStr[1] & 0x3Fu);
        if (cp >= 0xA0 && cp <= 0xFF) key = int(cp); // código do Qt::Key
    }
    if (!key) {
        std::string name = keyStr;
        for (char& c : name) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if      (name == "space")  key = 0x20;
        else if (name == "tab")    key = kKeyTab;
        else if (name == "backspace" || name == "back") key = kKeyBackspace;
        else if (name == "return") key = kKeyReturn;
        else if (name == "enter")  key = kKeyEnter;
        else if (name == "ins" || name == "insert")   key = kKeyInsert;
        else if (name == "del" || name == "delete")   key = kKeyDelete;
        else if (name == "pause")  key = kKeyPause;
        else if (name == "print")  key = kKeyPrint;
        else if (name == "home")   key = kKeyHome;
        else if (name == "end")    key = kKeyEnd;
        else if (name == "left")   key = kKeyLeft;
        else if (name == "up")     key = kKeyUp;
        else if (name == "right")  key = kKeyRight;
        else if (name == "down")   key = kKeyDown;
        else if (name == "pgup" || name == "pageup")   key = kKeyPageUp;
        else if (name == "pgdown" || name == "pagedown") key = kKeyPageDown;
        else if (name == "capslock")  key = kKeyCapsLock;
        else if (name == "numlock")   key = kKeyNumLock;
        else if (name == "scrolllock") key = kKeyScrollLock;
        else if (name.size() >= 2 && name[0] == 'f' &&
                 name.find_first_not_of("0123456789", 1) == std::string::npos) {
            const int n = atoi(name.c_str() + 1);
            if (n >= 1 && n <= 24) key = kKeyFBase + (n - 1);
        }
    }
    if (!key) return 0;
    return key | mods;
}

} // namespace hvk
