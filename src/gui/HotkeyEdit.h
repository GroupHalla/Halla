#pragma once

#include <QLineEdit>

class QFocusEvent;

// Campo que captura UMA tecla (ex.: "Space", "Ctrl+M") OU um botão de mouse
// (XButton1 = "Mouse4", XButton2 = "Mouse5", botão do meio = "MouseMeio").
// Usado para a tecla de PTT e nas teclas de atalho (o Halla permite
// botões laterais do mouse).
//
// A captura é feita em TRÊS camadas (Windows), então nenhum botão passa
// despercebido, mesmo com softwares de mouse que interceptam os XButtons:
//  1) eventos do widget (keyPressEvent/mousePressEvent) — caminho comum;
//  2) filtro de eventos do aplicativo — botões de mouse clicados sobre
//     qualquer widget do diálogo enquanto o campo está "armado" (focado);
//  3) filtro de eventos NATIVO (QAbstractNativeEventFilter) — lê WM_KEYDOWN,
//     WM_XBUTTONDOWN, WM_MBUTTONDOWN e WM_APPCOMMAND direto da fila do
//     Windows (cobre Logitech/Razer que enviam "Voltar/Avançar" do navegador).
class HotkeyEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit HotkeyEdit(QWidget* parent = nullptr);
    ~HotkeyEdit() override;

    QString spec() const { return m_spec; }
    void setSpec(const QString& spec);

    bool isArmed() const { return m_armed; }   // captura ativa (campo focado)
    void acceptSpec(const QString& spec);       // usado pelos filtros

    // nomes internos dos botões de mouse (independentes de tradução)
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
