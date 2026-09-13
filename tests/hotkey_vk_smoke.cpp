// ============================================================================
// Smoke do mapeamento hotkey spec -> VK (HotkeyVk.h, v1.1.27).
//
// Compila SEM Qt e SEM windows.h: os resolvedores de layout são mocks
// (US, ABNT2 e um "layout que não responde"). Valida:
//  - a reprodução do bug original: "\" (e toda pontuação OEM) caía fora da
//    tabela do conversor antigo e a tecla morria em silêncio;
//  - regressão: tudo o que o switch antigo mapeava continua igual;
//  - teclado numérico, Meta/Win, acentos Latin-1, fallback US;
//  - o parser manual "Mods+Tecla" (o que o QKeySequence::fromString rejeita).
// ============================================================================
#include "gui/HotkeyVk.h"

#include <cstdio>

static int g_fail = 0;
static int g_ok = 0;
#define CHECK(cond) do { \
    if (cond) { ++g_ok; } \
    else { ++g_fail; std::printf("FALHOU: %s (linha %d)\n", #cond, __LINE__); } \
} while (0)

// ---- mocks de VkKeyScanW ---------------------------------------------------

// layout US: pontuação nas posições OEM clássicas; '+' e '?' exigem shift
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
        // ç e acentos Latin-1 altos não existem como digitação única no US
        return -1;
    }
}

// layout ABNT2 (Brasil): 'ç' direto na tecla VK_OEM_1; '/' na tecla extra
// VK_ABNT_C1 ao lado do Shift direito
static int vkScanABNT(unsigned ch) {
    switch (ch) {
    case 0xE7: return 0xBA;  // ç -> VK_OEM_1 (posição do ';' no US)
    case '/':  return 0xC1;  // '/' -> VK_ABNT_C1 (tecla extra)
    default:   return vkScanUS(ch);
    }
}

// "layout que não responde" (força o fallback US embutido)
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
    // 1) O BUG ORIGINAL: "\" configurado como tecla (sussurro/PTT/atalho)
    //    era descartado em silêncio — "como se não apertasse a tecla".
    // ------------------------------------------------------------------
    CHECK(hvk::parseSpec("\\") == 0x5C);                    // Qt::Key_Backslash
    CHECK(specToVk("\\", &vkScanUS, vk, mods) && vk == 0xDC && mods == 0);
    CHECK(specToVk("\\", &vkScanABNT, vk, mods) && vk == 0xDC);
    CHECK(specToVk("Ctrl+\\", &vkScanUS, vk, mods) && vk == 0xDC &&
          (mods & hvk::kModControl));

    // ------------------------------------------------------------------
    // 2) Toda a pontuação OEM que faltava (layout US)
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

    // '+' e '?' exigem shift no layout: os mods vêm do resolvedor
    CHECK(specToVk("+", &vkScanUS, vk, mods) && vk == 0xBB && (mods & hvk::kModShift));
    CHECK(specToVk("?", &vkScanUS, vk, mods) && vk == 0xBF && (mods & hvk::kModShift));

    // ------------------------------------------------------------------
    // 3) Acentos Latin-1: 'ç' no ABNT2 (tecla direta, VK_OEM_1);
    //    no layout US o caractere não existe -> tecla recusada (não morta)
    // ------------------------------------------------------------------
    CHECK(hvk::parseSpec("\xC3\xA7") == 0xE7);              // "ç" em UTF-8
    CHECK(specToVk("\xC3\xA7", &vkScanABNT, vk, mods) && vk == 0xBA && mods == 0);
    CHECK(!specToVk("\xC3\xA7", &vkScanUS, vk, mods));
    CHECK(hvk::parseSpec("Ctrl+\xC3\xA7") == (hvk::kCtrlMod | 0xE7));

    // ------------------------------------------------------------------
    // 4) Fallback US embutido (resolvedor indisponível/falhou)
    // ------------------------------------------------------------------
    CHECK(specToVk("\\", &vkScanNone, vk, mods) && vk == 0xDC);
    CHECK(specToVk(",", &vkScanNone, vk, mods) && vk == 0xBC);
    CHECK(specToVk(";", &vkScanNone, vk, mods) && vk == 0xBA);
    CHECK(!specToVk("\xC3\xA7", &vkScanNone, vk, mods));    // ç não existe no US

    // ------------------------------------------------------------------
    // 5) Teclado numérico (Num+) — distinto da fileira principal
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
    // 6) Meta/Win: capturável e mapeável (MOD_WIN)
    // ------------------------------------------------------------------
    CHECK(hvk::parseSpec("Meta+X") == (hvk::kMetaMod | 0x58));
    CHECK(hvk::parseSpec("Win+X") == (hvk::kMetaMod | 0x58));
    CHECK(specToVk("Meta+X", &vkScanUS, vk, mods) && vk == 'X' &&
          (mods & hvk::kModWin) && !(mods & hvk::kModControl));

    // ------------------------------------------------------------------
    // 7) REGRESSÃO: cobertura do conversor antigo continua idêntica
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
    // tecla minúscula numa spec antiga: cai no caminho imprimível e resolve
    // a MESMA tecla física ('a' -> VK 'A')
    CHECK(specToVk("a", &vkScanUS, vk, mods) && vk == 0x41);

    // ------------------------------------------------------------------
    // 8) Parser: aceita o que o QKeySequence::fromString() rejeita
    // ------------------------------------------------------------------
    CHECK(hvk::parseSpec("") == 0);
    CHECK(hvk::parseSpec("Ctrl+") == 0);          // sem tecla
    CHECK(hvk::parseSpec("Foo") == 0);            // nome desconhecido
    CHECK(hvk::parseSpec("Ctrl+Foo") == 0);
    CHECK(hvk::parseSpec("Dead Acute") == 0);     // tecla morta nunca é spec
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
    std::printf("%d verificacoes OK, %d falhas\n", g_ok, g_fail);
    if (g_fail) {
        std::printf("SMOKE FALHOU\n");
        return 1;
    }
    std::printf("hotkey_vk_smoke: OK\n");
    return 0;
}
