#pragma once

#include <QLineEdit>

class QFocusEvent;

// Field that captures ONE key (e.g. "Space", "Ctrl+M") OR a mouse button
// (XButton1 = "Mouse4", XButton2 = "Mouse5", middle button = "MouseMiddle").
// Used for the PTT key and for the shortcuts (Halla allows side mouse buttons).
//
// Capture happens in THREE layers (Windows), so no button goes unnoticed even
// with mouse software that intercepts XButtons:
//  1) widget events (keyPressEvent/mousePressEvent) -- the common path;
//  2) application event filter -- mouse buttons clicked over any widget of the
//     dialog while the field is "armed" (focused);
//  3) NATIVE event filter (QAbstractNativeEventFilter) -- reads WM_KEYDOWN,
//     WM_XBUTTONDOWN, WM_MBUTTONDOWN and WM_APPCOMMAND straight from the Windows
//     queue (covers Logitech/Razer sending the browser's "Back/Forward").
class HotkeyEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit HotkeyEdit(QWidget* parent = nullptr);
    ~HotkeyEdit() override;

    QString spec() const { return m_spec; }
    void setSpec(const QString& spec);

    // builds the canonical "Mods+Key" spec from a Qt::Key + Qt mods. Used by both
    // capture layers (widget event and native filter) so it does NOT depend on
    // QKeySequence::toString -- which may omit the "Meta+" (Windows key) and
    // "Num+" (numeric keypad) modifiers. Format identical to what toString has
    // always produced ("Ctrl+F2", "Space", "\\"), only extended. Returns
    // QString() for a key without a spec (dead accents, exotic keys).
    static QString specFromKey(int key, int qtMods);

    bool isArmed() const { return m_armed; }   // captura ativa (campo focado)
    void acceptSpec(const QString& spec);       // usado pelos filtros

    // internal mouse button names (independent of translation)
    static constexpr const char* kMouse4      = "Mouse4";
    static constexpr const char* kMouse5      = "Mouse5";
    static constexpr const char* kMouseMiddle = "MouseMeio";

signals:
    void specChanged(const QString& spec);

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void setArmed(bool on);

    QString m_spec;
    bool m_armed = false;
    class NativeCapture* m_native = nullptr; // filtro nativo (Windows)
};

// "Is this shortcut action the whisper?" -- ABSOLUTE truth, shared by the
// options dialog (save) and by the dispatch (applyHotkeys /
// runConfiguredAction).
//
// The hotkey profile stores the action as a TRANSLATED string ("Whisper (hold
// to speak)" in EN, "Sussurrar ..." in ES). The old dispatch looked only for
// the PT substring "ussurr": in any other language the key fell into the
// generic shortcut path, which matches no action and silently ignores the
// press -- the user configured everything correctly and whisper "did not fire"
// (as if the button had never been pressed).
//
// Detection accepts the new canonical id ("whisper", the "id" field of the JSON
// since 1.1.26) or any spelling of the name in PT/EN/ES -- immune even to
// switching the client language after configuring it.
inline bool isWhisperHotkeyAction(const QString& action) {
    const QString a = action.toLower();
    return a.contains(QLatin1String("ussurr"))   // PT: "Sussurrar ..."
        || a.contains(QLatin1String("whisper"))  // EN + canonical "whisper" id
        || a.contains(QLatin1String("susur"));   // ES: "Susurrar/Susurro ..."
}
