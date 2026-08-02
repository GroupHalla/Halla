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
        case VK_ESCAPE: case VK_BACK: case VK_DELETE:
            return QStringLiteral("!clear");
        case VK_TAB:
            return QStringLiteral("!ignore"); // preserva a navegação do diálogo
        default: break;
        }

        int key = 0;
        if (vk >= 'A' && vk <= 'Z')      key = Qt::Key_A + int(vk - 'A');
        else if (vk >= '0' && vk <= '9') key = Qt::Key_0 + int(vk - '0');
        else if (vk >= VK_F1 && vk <= VK_F24) key = Qt::Key_F1 + int(vk - VK_F1);
        else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
            key = Qt::Key_0 + int(vk - VK_NUMPAD0);
        else switch (vk) {
        case VK_SPACE:    key = Qt::Key_Space;     break;
        case VK_RETURN:   key = Qt::Key_Return;    break;
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
        case VK_ADD:      key = Qt::Key_Plus;      break;
        case VK_SUBTRACT: key = Qt::Key_Minus;     break;
        case VK_MULTIPLY: key = Qt::Key_Asterisk;  break;
        case VK_DIVIDE:   key = Qt::Key_Slash;     break;
        case VK_DECIMAL:  key = Qt::Key_Period;    break;
        case VK_OEM_COMMA:  key = Qt::Key_Comma;     break;
        case VK_OEM_PERIOD: key = Qt::Key_Period;    break;
        case VK_OEM_MINUS:  key = Qt::Key_Minus;     break;
        case VK_OEM_PLUS:   key = Qt::Key_Equal;     break;
        default: return QStringLiteral("!ignore"); // tecla exótica não suportada
        }

        int mods = 0;
        if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= int(Qt::ShiftModifier);
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= int(Qt::ControlModifier);
        if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= int(Qt::AltModifier);
        return QKeySequence(key | mods).toString();
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

LRESULT CALLBACK GlobalMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && activeCaptureEdit) {
        if (wParam == WM_XBUTTONDOWN) {
            MSLLHOOKSTRUCT* hs = (MSLLHOOKSTRUCT*)lParam;
            int button = HIWORD(hs->mouseData);
            QString name = (button == XBUTTON1) ? QStringLiteral("Mouse4") : QStringLiteral("Mouse5");
            activeCaptureEdit->acceptSpec(name);
            activeCaptureEdit->clearFocus();
            return 1; // engole o clique
        }
        else if (wParam == WM_MBUTTONDOWN) {
            activeCaptureEdit->acceptSpec(QStringLiteral("MouseMeio"));
            activeCaptureEdit->clearFocus();
            return 1; // engole o clique
        }
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
    if (m_native) {
        qApp->removeNativeEventFilter(m_native);
        delete m_native;
    }
#endif
}

void HotkeyEdit::setSpec(const QString& spec) {
    m_spec = spec;
    // exibe botões do mouse no estilo Halla ("MOUSE BUTTON 5")
    if (spec == QLatin1String(kMouse4))            setText(QStringLiteral("MOUSE BUTTON 4"));
    else if (spec == QLatin1String(kMouse5))       setText(QStringLiteral("MOUSE BUTTON 5"));
    else if (spec == QLatin1String(kMouseMiddle))  setText(QStringLiteral("MOUSE BUTTON MIDDLE"));
    else                                           setText(spec);
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
        hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, GlobalMouseProc, GetModuleHandle(NULL), 0);
#endif
    } else {
        qApp->removeEventFilter(this);
        setPlaceholderText(tr("Clique aqui e pressione uma tecla ou botão do mouse"));
        setStyleSheet(QString());
#ifdef Q_OS_WIN
        if (hMouseHook) {
            UnhookWindowsHookEx(hMouseHook);
            hMouseHook = NULL;
        }
        if (activeCaptureEdit == this) {
            activeCaptureEdit = nullptr;
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
    if (key == Qt::Key_Escape || key == Qt::Key_Backspace || key == Qt::Key_Delete) {
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
    const int combined = key | int(e->modifiers() & (Qt::ShiftModifier |
                                 Qt::ControlModifier | Qt::AltModifier |
                                 Qt::MetaModifier));
    acceptSpec(QKeySequence(combined).toString());
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
