#pragma once

#include <QLineEdit>

// Campo que captura UMA tecla (ex.: "Space", "Ctrl+M") OU um botão de mouse
// (XButton1 = "Mouse4", XButton2 = "Mouse5", botão do meio = "MouseMeio").
// Usado para a tecla de PTT (o TeamSpeak permite botões laterais do mouse).
class HotkeyEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit HotkeyEdit(QWidget* parent = nullptr);

    QString spec() const { return m_spec; }
    void setSpec(const QString& spec);

    // nomes internos dos botões de mouse (independentes de tradução)
    static constexpr const char* kMouse4     = "Mouse4";
    static constexpr const char* kMouse5     = "Mouse5";
    static constexpr const char* kMouseMiddle = "MouseMeio";

signals:
    void specChanged(const QString& spec);

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    QString m_spec;
};
