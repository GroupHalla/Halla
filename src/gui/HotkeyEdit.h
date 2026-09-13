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

    // monta a spec canônica "Mods+Tecla" a partir de um Qt::Key + mods Qt.
    // Usada pelas duas camadas de captura (evento do widget e filtro nativo)
    // para NÃO depender do QKeySequence::toString — que pode omitir os
    // modificadores "Meta+" (tecla Windows) e "Num+" (teclado numérico).
    // Formato idêntico ao que o toString sempre gerou ("Ctrl+F2", "Space",
    // "\\"), apenas estendido. Devolve QString() para tecla sem spec
    // (acentos mortos, teclas exóticas).
    static QString specFromKey(int key, int qtMods);

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

// "Esta ação de atalho é o sussurro?" — verdade ABSOLUTA, compartilhada
// pelo diálogo de opções (gravação) e pelo dispatch (applyHotkeys /
// runConfiguredAction).
//
// O perfil de hotkeys grava a ação como string TRADUZIDA ("Sussurrar ..."
// em PT, "Whisper (hold to speak)" em EN, "Susurrar ..." em ES). O dispatch
// antigo procurava só o substring PT "ussurr": em qualquer outro idioma a
// tecla caía no caminho genérico de atalhos, que não casa com ação nenhuma
// e ignora o aperto em silêncio — o usuário configurava tudo certo e o
// sussurro "não disparava" (como se não apertasse o botão).
//
// A detecção aceita o id canônico novo ("whisper", campo "id" do JSON a
// partir do 1.1.26) ou qualquer grafia vivível do nome em PT/EN/ES — imune
// inclusive a trocar o idioma do cliente depois de configurar.
inline bool isWhisperHotkeyAction(const QString& action) {
    const QString a = action.toLower();
    return a.contains(QLatin1String("ussurr"))   // PT: "Sussurrar ..."
        || a.contains(QLatin1String("whisper"))  // EN + id canônico "whisper"
        || a.contains(QLatin1String("susur"));   // ES: "Susurrar/Susurro ..."
}
