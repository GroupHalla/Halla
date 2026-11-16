#pragma once

// ============================================================================
// HotkeyVk -- PORTABLE core of the "hotkey spec" <-> keys mapping.
//
// It depends neither on Qt nor on windows.h: the modifier bits and key codes
// used are the SAME stable Qt values (QKeyCombination::toCombined()), and the
// Windows VK/MOD_* values enter as literal constants. That allows compiling the
// mapping on any platform -- in particular the CI smoke tests (g++/cl without
// Qt) and the local Linux tests.
//
// WHY THIS EXISTS (v1.1.27): the old converter (MainWindow::specToVkImpl) only
// knew letters, digits, F1-F24 and control keys. Every punctuation/OEM key
// ("\", ";", "'", "[", "]", ",", "/", "=", "`", accented keys...) fell outside
// the table: vk stayed 0, the function returned false and the caller SILENTLY
// discarded the key. Capture worked (the field showed the key), but nothing was
// registered at runtime -- PTT, whisper and generic shortcuts stayed inert, "as
// if the user pressed nothing".
//
// Punctuation keys are precisely the most used as whisper/PTT keys on non-US
// layouts (ABNT2 in Brazil: "\" is a direct key, the cedilla key exists, "/"
// sits on the extra key next to the right Shift).
//
// The spec format is the same one QKeySequence::toString() has always produced
// ("Ctrl+F2", "Space", "A", "\\"), EXTENDED with the modifiers toString may
// omit: "Meta+" (Windows key) and "Num+" (numeric keypad).
// ============================================================================

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace hvk {

// ---- modifier bits (stable Qt::KeyboardModifier values) -------------------
constexpr int kShiftMod  = 0x02000000;
constexpr int kCtrlMod   = 0x04000000;
constexpr int kAltMod    = 0x08000000;
constexpr int kMetaMod   = 0x10000000; // tecla Windows/Command
constexpr int kKeypadMod = 0x20000000; // numeric keypad
constexpr int kModMask   = 0xFE000000;

// ---- control key codes (stable Qt::Key values) ----------------------------
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

// ---- Windows MOD_* (literal values from winuser.h) ------------------------
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

// Layout-dependent resolver: receives a character (Unicode <= 0xFF) and
// returns the VK in the low byte, with the 0x100/0x200/0x400 bits marking the
// shift/ctrl/alt the CURRENT layout requires to produce the character -- or -1
// if the character does not exist in the layout. On Windows the implementation
// is VkKeyScanW(); in the tests it is a mock (US layout, ABNT2, "empty"...).
using VkFromCharFn = int (*)(unsigned ch);

// ---------------------------------------------------------------------------
// US fallback for punctuation typed directly (no shift). Only used when the
// layout resolver fails or does not exist -- covers the case of the active
// layout not answering for the character, without leaving it dead.
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
// Converts the combined value (Qt mods | key, the QKeyCombination::toCombined
// layout) into a Windows VK + MOD_*.
//
// Coverage:
//  - everything the old converter already mapped (zero regression);
//  - printable Latin-1 punctuation/accent keys (0x20..0xFF): resolved IN THE
//    ACTIVE LAYOUT through vkFromChar (VkKeyScanW) -- "\\", ";", "`",
//    accented keys... land on the right physical key on any layout -- with a
//    US fallback;
//  - numeric keypad ("Num+5" -> VK_NUMPAD5, "Num++" -> VK_ADD...);
//  - Meta/Windows modifier -> MOD_WIN.
//
// `mods` leaves CLEAN (only real MOD_ALT/CONTROL/SHIFT/WIN, without
// MOD_NOREPEAT -- whoever registers the hotkey adds what it needs).
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

    // numeric keypad: distinct from the main row (with NumLock the reported VK
    // is VK_NUMPAD0..9, not the digit from the upper row)
    if (qtMods & kKeypadMod) {
        if (key >= '0' && key <= '9') { vk = kVkNumPad0 + unsigned(key - '0'); applyQtMods(); return true; }
        if (key == '+') { vk = kVkAdd;      applyQtMods(); return true; }
        if (key == '-') { vk = kVkSubtract; applyQtMods(); return true; }
        if (key == '*') { vk = kVkMultiply; applyQtMods(); return true; }
        if (key == '/') { vk = kVkDivide;   applyQtMods(); return true; }
        if (key == '.') { vk = kVkDecimal;  applyQtMods(); return true; }
        // "Num+Return"/"Num+Enter": numpad-enter reports a plain VK_RETURN --
        // it falls through as a normal Return below
    }

    // ASCII letters and digits (the VK matches the character code)
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
        // printable Latin-1 character: punctuation, accents, cedilla...
        if (key < 0x20 || key > 0xFF || key == 0x7F) return false;

        // 1) resolves IN THE ACTIVE LAYOUT: returns the right physical key
        //    (ABNT2, European...) and the mods the layout requires (e.g. "+"
        //    needs shift on US; AltGr enters as ctrl+alt and matches the
        //    physical polling)
        if (vkFromChar) {
            const int r = vkFromChar(unsigned(key));
            if (r >= 0) {
                vk = unsigned(r & 0xFF);
                if (r & 0x100) mods |= kModShift;
                if (r & 0x200) mods |= kModControl;
                if (r & 0x400) mods |= kModAlt;
            }
        }
        // 2) no layout answering: US table for directly typed characters
        if (!vk) vk = fallbackUsVk(key);
        if (!vk) return false; // exists neither in the layout nor in the fallback
        break;
    }
    }

    applyQtMods();
    return true;
}

// ---------------------------------------------------------------------------
// Manual parser for the "Mods+Key" spec -- handles everything
// QKeySequence::fromString() rejects (bare punctuation chars, "Num+5",
// accented keys...). Returns the combined value (mods | key) or 0 when it does
// not understand the spec.
//
// Format: tokens separated by "+"; the leading ones are modifiers
// (Ctrl/Control, Shift, Alt, Meta/Win/Super, Num/Keypad) and the last one is
// the key: a printable character (ASCII or Latin-1 in UTF-8) OR a name (Space,
// Tab, Backspace, Return, Enter, Ins/Insert, Del/Delete, Pause, Print, Home,
// End, Left, Up, Right, Down, PgUp/PageUp, PgDown/PageDown, CapsLock, NumLock,
// ScrollLock, F1..F24). A trailing "+" counts as the "+" key ("Ctrl++",
// "Num++").
inline int parseSpec(const char* utf8) {
    if (!utf8) return 0;
    std::string s(utf8);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (s.empty()) return 0;

    // separates "Mods+" (prefix) from the KEY (suffix). The '+' key is
    // ambiguous with the separator: "+", "Ctrl++", "Num++" -- it ends in "++"
    // (or is just "+").
    std::string keyStr;
    std::string modsStr;
    if (s.back() == '+') {
        if (s.size() == 1) {                          // tecla '+' pura
            keyStr = "+";
        } else if (s[s.size() - 2] == '+') {          // "Ctrl++" / "Num++"
            keyStr = "+";
            modsStr = s.substr(0, s.size() - 2);
        }
        // ends in '+' without "++": "Ctrl+" -- missing key, invalid
    } else {
        const size_t lastPlus = s.rfind('+');
        if (lastPlus == std::string::npos) keyStr = s;
        else {
            keyStr = s.substr(lastPlus + 1);
            modsStr = s.substr(0, lastPlus);
        }
    }
    if (keyStr.empty()) return 0;

    // modifiers: split the prefix on '+'; an empty token AT THE END is just the
    // pending separator ("Ctrl+Num+" for the '+' key); empty in the middle is an
    // error
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

    // the key: printable character or name
    int key = 0;
    if (keyStr.size() == 1) {
        const unsigned char c = (unsigned char)keyStr[0];
        if (c >= 0x21 && c <= 0x7E) key = int(c); // printable ASCII (not space)
    } else if (keyStr.size() == 2 &&
               (unsigned char)keyStr[0] >= 0xC2 && (unsigned char)keyStr[0] <= 0xC5) {
        // Latin-1 supplement in UTF-8 (accented characters): 110xxxxx 10xxxxxx
        const unsigned cp = ((unsigned char)keyStr[0] & 0x1Fu) << 6 |
                            ((unsigned char)keyStr[1] & 0x3Fu);
        if (cp >= 0xA0 && cp <= 0xFF) key = int(cp); // Qt::Key code
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
