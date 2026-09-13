#include "HotkeyEdit.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QFocusEvent>
#include <QKeySequence>
#include <QApplication>

#ifdef Q_OS_WIN
#include <QAbstractNativeEventFilter>
#include <windows.h>
#endif

// ============================================================================
// Filtro NATIVO (Windows): enxerga os eventos antes de qualquer widget.
// Cobre botões laterais mesmo quando algum software do mouse consome os
// cliques ou os converte em "Voltar/Avançar" do navegador (WM_APPCOMMAND)
// ou em VK_BROWSER_* (chegam como tecla).
// ============================================================================
#ifdef Q_OS_WIN
class NativeCapture : public QAbstractNativeEventFilter {
public:
    HotkeyEdit* edit = nullptr;

    static QString specFromVk(WPARAM vk) {
        // modificadores sozinhos: aguarda a tecla real
        switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
            return QString();
        // botões de mouse que chegam como "tecla" (softwares de mouse)
        case VK_XBUTTON1: case VK_BROWSER_BACK:
            return QString::fromLatin1(HotkeyEdit::kMouse4);
        case VK_XBUTTON2: case VK_BROWSER_FORWARD:
            return QString::fromLatin1(HotkeyEdit::kMouse5);
        case VK_MBUTTON:
            return QString::fromLatin1(HotkeyEdit::kMouseMiddle);
        case VK_ESCAPE:
            return QStringLiteral("!clear");
        case VK_TAB:
            return QStringLiteral("!ignore"); // preserva a navegação do diálogo
        default: break;
        }

        int key = 0;
        int extraMods = 0; // KeypadModifier (teclado numérico)
        if (vk >= 'A' && vk <= 'Z')      key = Qt::Key_A + int(vk - 'A');
        else if (vk >= '0' && vk <= '9') key = Qt::Key_0 + int(vk - '0');
        else if (vk >= VK_F1 && vk <= VK_F24) key = Qt::Key_F1 + int(vk - VK_F1);
        else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
            // v1.1.27: numpad deixa de colidir com a fileira de cima —
            // "Num+5" resolve VK_NUMPAD5 no runtime (ver gui/HotkeyVk.h)
            key = Qt::Key_0 + int(vk - VK_NUMPAD0);
            extraMods = int(Qt::KeypadModifier);
        }
        else switch (vk) {
        case VK_SPACE:    key = Qt::Key_Space;     break;
        case VK_RETURN:   key = Qt::Key_Return;    break;
        // v1.1.27: Backspace/Delete viraram TECLA configurável (o runtime
        // sempre soube mapeá-las; só a captura descartava/limpava)
        case VK_BACK:     key = Qt::Key_Backspace; break;
        case VK_DELETE:   key = Qt::Key_Delete;    break;
        case VK_INSERT:   key = Qt::Key_Insert;    break;
        case VK_HOME:     key = Qt::Key_Home;      break;
        case VK_END:      key = Qt::Key_End;       break;
        case VK_PRIOR:    key = Qt::Key_PageUp;    break;
        case VK_NEXT:     key = Qt::Key_PageDown;  break;
        case VK_LEFT:     key = Qt::Key_Left;      break;
        case VK_UP:       key = Qt::Key_Up;        break;
        case VK_RIGHT:    key = Qt::Key_Right;     break;
        case VK_DOWN:     key = Qt::Key_Down;      break;
        case VK_CAPITAL:  key = Qt::Key_CapsLock;  break;
        case VK_NUMLOCK:  key = Qt::Key_NumLock;   break;
        case VK_SCROLL:   key = Qt::Key_ScrollLock;break;
        case VK_SNAPSHOT: key = Qt::Key_Print;     break;
        case VK_PAUSE:    key = Qt::Key_Pause;     break;
        // operações do teclado numérico (só existem lá — levam "Num+")
        case VK_ADD:      key = Qt::Key_Plus;     extraMods = int(Qt::KeypadModifier); break;
        case VK_SUBTRACT: key = Qt::Key_Minus;    extraMods = int(Qt::KeypadModifier); break;
        case VK_MULTIPLY: key = Qt::Key_Asterisk; extraMods = int(Qt::KeypadModifier); break;
        case VK_DIVIDE:   key = Qt::Key_Slash;    extraMods = int(Qt::KeypadModifier); break;
        case VK_DECIMAL:  key = Qt::Key_Period;   extraMods = int(Qt::KeypadModifier); break;
        default: {
            // v1.1.27: teclas OEM (pontuação/acentos — "\", ";", "ç", "'" ...)
            // resolvem o CARACTERE no LAYOUT ATIVO em vez de uma tabela fixa
            // US: a spec gravada é o caractere da tecla física CERTA em
            // ABNT2 e afins, e o runtime re-resolve via VkKeyScanW
            // (gui/HotkeyVk.h). Os cases OEM fixos da versão anterior cobriam
            // só 4 teclas e ainda por cima na posição do layout americano.
            const UINT mc = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_CHAR);
            if (mc == 0 || (mc & 0x80008000u))
                return QStringLiteral("!ignore"); // tecla morta ou exótica
            const WCHAR ch = WCHAR(mc & 0xFFFF);
            if (ch >= 0x20 && ch != 0x7F && ch <= 0xFF)
                key = int(ch); // Qt::Key imprimível = código Latin-1 do caractere
            else
                return QStringLiteral("!ignore");
            break;
        }
        }

        int mods = extraMods;
        if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= int(Qt::ShiftModifier);
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= int(Qt::ControlModifier);
        if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= int(Qt::AltModifier);
        // v1.1.27: a tecla Windows entra na spec — antes era descartada em
        // silêncio aqui e "Win+X" era gravado como "X" puro
        if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000)
            mods |= int(Qt::MetaModifier);

        // monta a spec sem depender do toString do Qt (garante "Meta+"/"Num+")
        const QString spec = HotkeyEdit::specFromKey(key, mods);
        return spec.isEmpty() ? QStringLiteral("!ignore") : spec;
    }

    bool nativeEventFilter(const QByteArray& type, void* message,
                           qintptr* result) override {
        if (!edit || !edit->isArmed()) return false;
        if (type != QByteArrayLiteral("windows_generic_MSG") &&
            type != QByteArrayLiteral("windows_dispatcher_MSG")) return false;
        MSG* msg = static_cast<MSG*>(message);

        switch (msg->message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const QString s = specFromVk(msg->wParam);
            if (s == QLatin1String("!ignore")) return false; // deixa passar (Tab etc.)
            if (result) *result = 0;
            if (s.isEmpty()) return true;                    // modificador sozinho: engole
            if (s == QLatin1String("!clear")) edit->acceptSpec(QString());
            else                              edit->acceptSpec(s);
            return true;
        }
        case WM_XBUTTONDOWN: {
            const WORD b = HIWORD(msg->wParam);
            edit->acceptSpec(QString::fromLatin1(b == XBUTTON1 ? HotkeyEdit::kMouse4
                                                              : HotkeyEdit::kMouse5));
            edit->clearFocus();
            if (result) *result = TRUE;
            return true;
        }
        case WM_MBUTTONDOWN:
            edit->acceptSpec(QString::fromLatin1(HotkeyEdit::kMouseMiddle));
            edit->clearFocus();
            if (result) *result = 0;
            return true;
        case WM_APPCOMMAND: {
            // mouses que enviam comandos de "Voltar/Avançar" do navegador
            const int cmd = GET_APPCOMMAND_LPARAM(msg->lParam);
            if (cmd == APPCOMMAND_BROWSER_BACKWARD) {
                edit->acceptSpec(QString::fromLatin1(HotkeyEdit::kMouse4));
                edit->clearFocus();
                if (result) *result = TRUE;
                return true;
            }
            if (cmd == APPCOMMAND_BROWSER_FORWARD) {
                edit->acceptSpec(QString::fromLatin1(HotkeyEdit::kMouse5));
                edit->clearFocus();
                if (result) *result = TRUE;
                return true;
            }
            return false;
        }
        default: return false;
        }
    }
};

static HHOOK hMouseHook = NULL;
static HotkeyEdit* activeCaptureEdit = nullptr;

// Hook de baixo nível: o caminho crítico do input do sistema INTEIRO passa
// por aqui enquanto um campo de captura está armado. Por isso ele só faz o
// mínimo — identifica o botão e ENFILEIRA o processamento (invokeMethod
// com QueuedConnection volta ao loop da GUI depois que o hook retorna).
// As versões anteriores chamavam setText/emit/clearFocus e até
// UnhookWindowsHookEx DENTRO do hook: além do risco de reentrância, qualquer
// demora aqui (ex.: sync do QSettings num slot conectado) travava o mouse
// do sistema inteiro até o Windows descartar o hook por timeout.
LRESULT CALLBACK GlobalMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && activeCaptureEdit
        && (wParam == WM_XBUTTONDOWN || wParam == WM_MBUTTONDOWN)) {
        const int button = (wParam == WM_MBUTTONDOWN) ? 3
            : ((HIWORD(reinterpret_cast<MSLLHOOKSTRUCT*>(lParam)->mouseData) == XBUTTON1) ? 4 : 5);
        HotkeyEdit* const target = activeCaptureEdit;
        QMetaObject::invokeMethod(target, [target, button] {
            // fora do hook: pode tocar Qt, mudar foco e desarmar com segurança
            switch (button) {
            case 3: target->acceptSpec(QString::fromLatin1(HotkeyEdit::kMouseMiddle)); break;
            case 4: target->acceptSpec(QString::fromLatin1(HotkeyEdit::kMouse4));      break;
            default: target->acceptSpec(QString::fromLatin1(HotkeyEdit::kMouse5));     break;
            }
            target->clearFocus(); // desarma via focusOutEvent
        }, Qt::QueuedConnection);
        return 1; // engole o clique: ele está sendo capturado como atalho
    }
    return CallNextHookEx(hMouseHook, nCode, wParam, lParam);
}
#endif

// ============================================================================
HotkeyEdit::HotkeyEdit(QWidget* parent) : QLineEdit(parent) {
    setReadOnly(true);
    setClearButtonEnabled(false);
    setFocusPolicy(Qt::StrongFocus);
    setPlaceholderText(tr("Clique aqui e pressione uma tecla ou botão do mouse"));
#ifdef Q_OS_WIN
    m_native = new NativeCapture;
    m_native->edit = this;
    qApp->installNativeEventFilter(m_native);
#endif
}

HotkeyEdit::~HotkeyEdit() {
#ifdef Q_OS_WIN
    // Desarma ANTES de destruir: se o campo estava com foco quando o diálogo
    // fechou, o hook LL de mouse ficava instalado para sempre e
    // activeCaptureEdit apontava para este objeto já morto — o próximo clique
    // de botão lateral/meio em QUALQUER app era use-after-free (crash
    // aleatório) e o hook vazado engrossava o input do sistema a cada
    // travada da GUI.
    if (m_armed) setArmed(false);
    if (m_native) {
        qApp->removeNativeEventFilter(m_native);
        delete m_native;
    }
#endif
}

void HotkeyEdit::setSpec(const QString& spec) {
    m_spec = spec;
    // exibe botões do mouse no estilo Halla ("MOUSE BUTTON 5")
    if (spec == QLatin1String(kMouse4))            setText(tr("BOTÃO DO MOUSE 4"));
    else if (spec == QLatin1String(kMouse5))       setText(tr("BOTÃO DO MOUSE 5"));
    else if (spec == QLatin1String(kMouseMiddle))  setText(tr("BOTÃO CENTRAL DO MOUSE"));
    else                                           setText(spec);
}

// monta a spec canônica "Mods+Tecla" (formato do QKeySequence::toString,
// estendido com "Meta+"/"Num+"). Ver declaração em HotkeyEdit.h.
QString HotkeyEdit::specFromKey(int key, int qtMods) {
    QString prefix;
    if (qtMods & int(Qt::ControlModifier)) prefix += QStringLiteral("Ctrl+");
    if (qtMods & int(Qt::AltModifier))     prefix += QStringLiteral("Alt+");
    if (qtMods & int(Qt::ShiftModifier))   prefix += QStringLiteral("Shift+");
    if (qtMods & int(Qt::MetaModifier))    prefix += QStringLiteral("Meta+");
    if (qtMods & int(Qt::KeypadModifier))  prefix += QStringLiteral("Num+");
    QString name;
    if (key >= 0x21 && key <= 0xFF && key != 0x7F) {
        name = QString(QChar(ushort(key))); // imprimível: o próprio caractere
    } else switch (key) {
    case Qt::Key_Space:     name = QStringLiteral("Space"); break;
    case Qt::Key_Tab:       name = QStringLiteral("Tab"); break;
    case Qt::Key_Backspace: name = QStringLiteral("Backspace"); break;
    case Qt::Key_Return:    name = QStringLiteral("Return"); break;
    case Qt::Key_Enter:     name = QStringLiteral("Enter"); break;
    case Qt::Key_Insert:    name = QStringLiteral("Ins"); break;
    case Qt::Key_Delete:    name = QStringLiteral("Del"); break;
    case Qt::Key_Pause:     name = QStringLiteral("Pause"); break;
    case Qt::Key_Print:     name = QStringLiteral("Print"); break;
    case Qt::Key_Home:      name = QStringLiteral("Home"); break;
    case Qt::Key_End:       name = QStringLiteral("End"); break;
    case Qt::Key_Left:      name = QStringLiteral("Left"); break;
    case Qt::Key_Up:        name = QStringLiteral("Up"); break;
    case Qt::Key_Right:     name = QStringLiteral("Right"); break;
    case Qt::Key_Down:      name = QStringLiteral("Down"); break;
    case Qt::Key_PageUp:    name = QStringLiteral("PgUp"); break;
    case Qt::Key_PageDown:  name = QStringLiteral("PgDown"); break;
    case Qt::Key_CapsLock:  name = QStringLiteral("CapsLock"); break;
    case Qt::Key_NumLock:   name = QStringLiteral("NumLock"); break;
    case Qt::Key_ScrollLock:name = QStringLiteral("ScrollLock"); break;
    default:
        if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
            name = QStringLiteral("F") + QString::number(key - Qt::Key_F1 + 1);
        break; // sem spec (acento morto, tecla exótica): vazio
    }
    if (name.isEmpty()) return QString();
    return prefix + name;
}

void HotkeyEdit::acceptSpec(const QString& spec) {
    setSpec(spec);
    emit specChanged(m_spec);
}

void HotkeyEdit::setArmed(bool on) {
    if (m_armed == on) return;
    m_armed = on;
    if (on) {
        // enquanto armado, cliques de mouse em QUALQUER widget do app contam
        qApp->installEventFilter(this);
        setPlaceholderText(tr("Pressione uma tecla ou botão do mouse...  (Esc limpa)"));
        setStyleSheet(QStringLiteral("HotkeyEdit { border: 1px solid #0078D7; }"));
#ifdef Q_OS_WIN
        activeCaptureEdit = this;
        // um único hook para o app todo (dois campos armados ao mesmo tempo
        // não devem instalar dois hooks — o segundo sobrescreveria o
        // handle do primeiro e o hook antigo vazaria instalado)
        if (!hMouseHook)
            hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, GlobalMouseProc, GetModuleHandle(NULL), 0);
#endif
    } else {
        qApp->removeEventFilter(this);
        setPlaceholderText(tr("Clique aqui e pressione uma tecla ou botão do mouse"));
        setStyleSheet(QString());
#ifdef Q_OS_WIN
        if (activeCaptureEdit == this) {
            activeCaptureEdit = nullptr;
        }
        if (hMouseHook) {
            UnhookWindowsHookEx(hMouseHook);
            hMouseHook = NULL;
        }
#endif
    }
}

// ---- camada 2: eventos de mouse no aplicativo inteiro (enquanto armado)
bool HotkeyEdit::eventFilter(QObject* obj, QEvent* ev) {
    if (m_armed && ev->type() == QEvent::MouseButtonPress) {
        QMouseEvent* me = static_cast<QMouseEvent*>(ev);
        QString name;
        switch (me->button()) {
        case Qt::XButton1:     name = QString::fromLatin1(kMouse4);      break;
        case Qt::XButton2:     name = QString::fromLatin1(kMouse5);      break;
        case Qt::MiddleButton: name = QString::fromLatin1(kMouseMiddle); break;
        default: break;
        }
        if (!name.isEmpty()) {
            acceptSpec(name);
            clearFocus(); // desarma imediatamente ao capturar!
            return true; // consome: evita navegação/fechar diálogo com o botão
        }
    }
    return QLineEdit::eventFilter(obj, ev);
}

// ---- camada 1: eventos diretos do widget
void HotkeyEdit::keyPressEvent(QKeyEvent* e) {
    const int key = e->key();
    // v1.1.27: só Esc limpa — Backspace/Delete são teclas de PTT legítimas
    // (o runtime sempre soube mapeá-las; a captura é que as devorava)
    if (key == Qt::Key_Escape) {
        acceptSpec(QString());
        clearFocus(); // desarma imediatamente!
        e->accept();
        return;
    }
    if (key == Qt::Key_Tab || key == Qt::Key_Backtab) { // preserva navegação
        QLineEdit::keyPressEvent(e);
        return;
    }
    // ignora modificadores soltos (aguarda a tecla real)
    if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt ||
        key == Qt::Key_Meta || key == Qt::Key_unknown || key <= 0) {
        e->accept();
        return;
    }
    // v1.1.27: KeypadModifier entra na spec ("Num+5") e a montagem é manual
    // (specFromKey) — o QKeySequence::toString pode omitir Num/Meta
    const int mods = int(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier |
                     Qt::AltModifier | Qt::MetaModifier | Qt::KeypadModifier));
    const QString spec = specFromKey(key, mods);
    if (spec.isEmpty()) {
        e->accept(); // tecla sem spec (acento morto, exótica): aguarda a próxima
        return;
    }
    acceptSpec(spec);
    clearFocus(); // desarma imediatamente ao capturar!
    e->accept();
}

void HotkeyEdit::mousePressEvent(QMouseEvent* e) {
    QString name;
    switch (e->button()) {
    case Qt::XButton1:     name = QString::fromLatin1(kMouse4);      break;
    case Qt::XButton2:     name = QString::fromLatin1(kMouse5);      break;
    case Qt::MiddleButton: name = QString::fromLatin1(kMouseMiddle); break;
    default: break;
    }
    if (!name.isEmpty()) {
        acceptSpec(name);
        clearFocus(); // desarma imediatamente ao capturar!
        e->accept();
        return;
    }
    QLineEdit::mousePressEvent(e);
}

void HotkeyEdit::focusInEvent(QFocusEvent* e) {
    setArmed(true);
    QLineEdit::focusInEvent(e);
}

void HotkeyEdit::focusOutEvent(QFocusEvent* e) {
    setArmed(false);
    QLineEdit::focusOutEvent(e);
}
