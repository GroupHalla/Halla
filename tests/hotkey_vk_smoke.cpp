// ============================================================================
// Smoke test for the hotkey spec -> VK mapping (HotkeyVk.h, v1.1.27).
//
// Compiles WITHOUT Qt and WITHOUT windows.h: the layout resolvers are mocks
// (US, ABNT2 and a "layout that never answers"). It validates:
//  - the original bug: "\\" (and all OEM punctuation) fell outside the old
//    converter table and the key died silently;
//  - regression: everything the old switch mapped is still identical;
//  - numeric keypad, Meta/Win, Latin-1 accents, US fallback;
//  - the hand-written "Mods+Key" parser (what QKeySequence::fromString
//    rejects).
// ============================================================================
#include "gui/HotkeyVk.h"

#include <cstdio>

static int g_fail = 0;
static int g_ok = 0;
#define CHECK(cond) do { \
    if (cond) { ++g_ok; } \
    else { ++g_fail; std::printf("FAILED: %s (line %d)\n", #cond, __LINE__); } \
} while (0)

// ---- VkKeyScanW mocks ------------------------------------------------------

// US layout: punctuation on the classic OEM positions; '+' and '?' need shift
static int vkScanUS(unsigned ch) {
    switch (ch) {
    case '\\': return 0xDC;
    case ';':  return 0xBA;
    case '\'': return 0xDE;
    case '`':  return 0xC0;
    case '[':  return 0xDB;
    case ']':  return 0xDD;
    case ',':  return 0xBC;
    case '.':  return 0xBE;
    case '/':  return 0xBF;
    case '-':  return 0xBD;
    case '=':  return 0xBB;
    case '+':  return 0xBB | 0x100; // '+' = shift + tecla '='
    case '?':  return 0xBF | 0x100; // '?' = shift + tecla '/'
    default:
        if (ch >= 'a' && ch <= 'z') return int(ch) - 0x20; // 'a' -> VK 'A'
        if (ch >= '0' && ch <= '9') return int(ch);
        if (ch == ' ') return 0x20;
        // 'c' cedilla and high Latin-1 accents are not single-key input on US
        return -1;
    }
}

// ABNT2 layout (Brazil): 'c' cedilla on the VK_OEM_1 key; '/' on the extra key
// VK_ABNT_C1 ao lado do Shift direito
static int vkScanABNT(unsigned ch) {
    switch (ch) {
    case 0xE7: return 0xBA;  // c cedilla -> VK_OEM_1 (where ';' sits on US)
    case '/':  return 0xC1;  // '/' -> VK_ABNT_C1 (tecla extra)
    default:   return vkScanUS(ch);
    }
}

// "layout that never answers" (forces the built-in US fallback)
static int vkScanNone(unsigned) { return -1; }

// helper: spec (string crua) -> vk/mods pelo caminho completo
static bool specToVk(const char* s, hvk::VkFromCharFn fn, unsigned& vk, unsigned& mods) {
    const int combined = hvk::parseSpec(s);
    if (!combined) return false;
    return hvk::specToVkCore(combined, fn, vk, mods);
}

int main() {
    unsigned vk = 0, mods = 0;

    // ------------------------------------------------------------------
    // 1) THE ORIGINAL BUG: "\" bound as a key (whisper/PTT/hotkey) was
    //    silently discarded -- "as if the key was never pressed".
    // ------------------------------------------------------------------
    CHECK(hvk::parseSpec("\\") == 0x5C);                    // Qt::Key_Backslash
    CHECK(specToVk("\\", &vkScanUS, vk, mods) && vk == 0xDC && mods == 0);
    CHECK(specToVk("\\", &vkScanABNT, vk, mods) && vk == 0xDC);
    CHECK(specToVk("Ctrl+\\", &vkScanUS, vk, mods) && vk == 0xDC &&
          (mods & hvk::kModControl));

    // ------------------------------------------------------------------
    // 2) All the OEM punctuation that was missing (US layout)
    // ------------------------------------------------------------------
    CHECK(specToVk(";", &vkScanUS, vk, mods)  && vk == 0xBA);
    CHECK(specToVk("'", &vkScanUS, vk, mods)  && vk == 0xDE);
    CHECK(specToVk("`", &vkScanUS, vk, mods)  && vk == 0xC0);
    CHECK(specToVk("[", &vkScanUS, vk, mods)  && vk == 0xDB);
    CHECK(specToVk("]", &vkScanUS, vk, mods)  && vk == 0xDD);
    CHECK(specToVk(",", &vkScanUS, vk, mods)  && vk == 0xBC);
    CHECK(specToVk(".", &vkScanUS, vk, mods)  && vk == 0xBE);
    CHECK(specToVk("/", &vkScanUS, vk, mods)  && vk == 0xBF);
    CHECK(specToVk("-", &vkScanUS, vk, mods)  && vk == 0xBD);
    CHECK(specToVk("=", &vkScanUS, vk, mods)  && vk == 0xBB);

    // '+' and '?' need shift in the layout: the mods come from the resolver
    CHECK(specToVk("+", &vkScanUS, vk, mods) && vk == 0xBB && (mods & hvk::kModShift));
    CHECK(specToVk("?", &vkScanUS, vk, mods) && vk == 0xBF && (mods & hvk::kModShift));

    // ------------------------------------------------------------------
    // 3) Latin-1 accents: 'c' cedilla on ABNT2 (direct key, VK_OEM_1); on the
    //    US layout the character does not exist -> key refused (not dead)
    // ------------------------------------------------------------------
    CHECK(hvk::parseSpec("\xC3\xA7") == 0xE7);              // cedilla as UTF-8
    CHECK(specToVk("\xC3\xA7", &vkScanABNT, vk, mods) && vk == 0xBA && mods == 0);
    CHECK(!specToVk("\xC3\xA7", &vkScanUS, vk, mods));
    CHECK(hvk::parseSpec("Ctrl+\xC3\xA7") == (hvk::kCtrlMod | 0xE7));

    // ------------------------------------------------------------------
    // 4) Built-in US fallback (resolver unavailable/failed)
    // ------------------------------------------------------------------
    CHECK(specToVk("\\", &vkScanNone, vk, mods) && vk == 0xDC);
    CHECK(specToVk(",", &vkScanNone, vk, mods) && vk == 0xBC);
    CHECK(specToVk(";", &vkScanNone, vk, mods) && vk == 0xBA);
    CHECK(!specToVk("\xC3\xA7", &vkScanNone, vk, mods));    // no cedilla on US

    // ------------------------------------------------------------------
    // 5) Numeric keypad (Num+) -- distinct from the top row
    // ------------------------------------------------------------------
    CHECK(hvk::parseSpec("Num+5") == (hvk::kKeypadMod | 0x35));
    CHECK(specToVk("Num+5", &vkScanUS, vk, mods) && vk == 0x65);       // VK_NUMPAD5
    CHECK(specToVk("5", &vkScanUS, vk, mods) && vk == 0x35);           // fileira de cima
    CHECK(specToVk("Num++", &vkScanUS, vk, mods) && vk == 0x6B);       // VK_ADD
    CHECK(specToVk("Num+-", &vkScanUS, vk, mods) && vk == 0x6D);       // VK_SUBTRACT
    CHECK(specToVk("Num+*", &vkScanUS, vk, mods) && vk == 0x6A);       // VK_MULTIPLY
    CHECK(specToVk("Num+/", &vkScanUS, vk, mods) && vk == 0x6F);       // VK_DIVIDE
    CHECK(specToVk("Num+.", &vkScanUS, vk, mods) && vk == 0x6E);       // VK_DECIMAL
    CHECK(hvk::parseSpec("Ctrl++") == (hvk::kCtrlMod | 0x2B));         // Ctrl e tecla '+'
    CHECK(hvk::parseSpec("+") == 0x2B);                                // '+' puro

    // ------------------------------------------------------------------
    // 6) Meta/Win: capturable and mappable (MOD_WIN)
    // ------------------------------------------------------------------
    CHECK(hvk::parseSpec("Meta+X") == (hvk::kMetaMod | 0x58));
    CHECK(hvk::parseSpec("Win+X") == (hvk::kMetaMod | 0x58));
    CHECK(specToVk("Meta+X", &vkScanUS, vk, mods) && vk == 'X' &&
          (mods & hvk::kModWin) && !(mods & hvk::kModControl));

    // ------------------------------------------------------------------
    // 7) REGRESSION: the old converter coverage is still identical
    // ------------------------------------------------------------------
    CHECK(specToVk("A", &vkScanUS, vk, mods) && vk == 0x41);
    CHECK(specToVk("Z", &vkScanUS, vk, mods) && vk == 0x5A);
    CHECK(specToVk("0", &vkScanUS, vk, mods) && vk == 0x30);
    CHECK(specToVk("9", &vkScanUS, vk, mods) && vk == 0x39);
    CHECK(specToVk("Space", &vkScanUS, vk, mods) && vk == 0x20);
    CHECK(specToVk("Tab", &vkScanUS, vk, mods) && vk == 0x09);
    CHECK(specToVk("CapsLock", &vkScanUS, vk, mods) && vk == 0x14);
    CHECK(specToVk("Return", &vkScanUS, vk, mods) && vk == 0x0D);
    CHECK(specToVk("Backspace", &vkScanUS, vk, mods) && vk == 0x08);
    CHECK(specToVk("Ins", &vkScanUS, vk, mods) && vk == 0x2D);
    CHECK(specToVk("Insert", &vkScanUS, vk, mods) && vk == 0x2D);
    CHECK(specToVk("Del", &vkScanUS, vk, mods) && vk == 0x2E);
    CHECK(specToVk("Delete", &vkScanUS, vk, mods) && vk == 0x2E);
    CHECK(specToVk("Home", &vkScanUS, vk, mods) && vk == 0x24);
    CHECK(specToVk("End", &vkScanUS, vk, mods) && vk == 0x23);
    CHECK(specToVk("PgUp", &vkScanUS, vk, mods) && vk == 0x21);
    CHECK(specToVk("PageUp", &vkScanUS, vk, mods) && vk == 0x21);
    CHECK(specToVk("PgDown", &vkScanUS, vk, mods) && vk == 0x22);
    CHECK(specToVk("Print", &vkScanUS, vk, mods) && vk == 0x2C);
    CHECK(specToVk("Pause", &vkScanUS, vk, mods) && vk == 0x13);
    CHECK(specToVk("Left", &vkScanUS, vk, mods) && vk == 0x25);
    CHECK(specToVk("Up", &vkScanUS, vk, mods) && vk == 0x26);
    CHECK(specToVk("Right", &vkScanUS, vk, mods) && vk == 0x27);
    CHECK(specToVk("Down", &vkScanUS, vk, mods) && vk == 0x28);
    CHECK(specToVk("F1", &vkScanUS, vk, mods) && vk == 0x70);
    CHECK(specToVk("F12", &vkScanUS, vk, mods) && vk == 0x7B);
    CHECK(specToVk("F24", &vkScanUS, vk, mods) && vk == 0x87);
    CHECK(specToVk("Ctrl+F2", &vkScanUS, vk, mods) && vk == 0x71 &&
          (mods & hvk::kModControl));
    CHECK(specToVk("Shift+A", &vkScanUS, vk, mods) && vk == 0x41 &&
          (mods & hvk::kModShift));
    CHECK(specToVk("Alt+Q", &vkScanUS, vk, mods) && vk == 'Q' &&
          (mods & hvk::kModAlt));
    // lowercase key in an old spec: falls through the printable path and
    // resolves to the SAME physical key ('a' -> VK 'A')
    CHECK(specToVk("a", &vkScanUS, vk, mods) && vk == 0x41);

    // ------------------------------------------------------------------
    // 8) Parser: aceita o que o QKeySequence::fromString() rejeita
    // ------------------------------------------------------------------
    CHECK(hvk::parseSpec("") == 0);
    CHECK(hvk::parseSpec("Ctrl+") == 0);          // sem tecla
    CHECK(hvk::parseSpec("Foo") == 0);            // nome desconhecido
    CHECK(hvk::parseSpec("Ctrl+Foo") == 0);
    CHECK(hvk::parseSpec("Dead Acute") == 0);     // a dead key is never a spec
    CHECK(hvk::parseSpec("Backspace") == hvk::kKeyBackspace);
    CHECK(hvk::parseSpec("Enter") == hvk::kKeyEnter);
    CHECK(hvk::parseSpec("NumLock") == hvk::kKeyNumLock);
    CHECK(hvk::parseSpec("ScrollLock") == hvk::kKeyScrollLock);
    CHECK(hvk::parseSpec("F13") == hvk::kKeyFBase + 12);
    CHECK(hvk::parseSpec("ctrl+alt+\\") == (hvk::kCtrlMod | hvk::kAltMod | 0x5C));
    CHECK(hvk::parseSpec("SHIFT+A") == (hvk::kShiftMod | 0x41)); // caixa alta
    // AltGr (ctrl+alt) casa com o que o Windows reporta fisicamente
    CHECK(specToVk("Ctrl+Alt+,", &vkScanUS, vk, mods) && vk == 0xBC &&
          (mods & hvk::kModControl) && (mods & hvk::kModAlt));

    // ------------------------------------------------------------------
    // 9) Modificadores combinados com Num+ (ordem livre)
    // ------------------------------------------------------------------
    CHECK(hvk::parseSpec("Ctrl+Num+5") == (hvk::kCtrlMod | hvk::kKeypadMod | 0x35));
    CHECK(specToVk("Ctrl+Num+5", &vkScanUS, vk, mods) && vk == 0x65 &&
          (mods & hvk::kModControl));

    // ------------------------------------------------------------------
    std::printf("%d checks OK, %d failures\n", g_ok, g_fail);
    if (g_fail) {
        std::printf("SMOKE FAILED\n");
        return 1;
    }
    std::printf("hotkey_vk_smoke: OK\n");
    return 0;
}
