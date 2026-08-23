#pragma once

#include <QDialog>

// Janela "Sobre o Halla" com faixa, logo e informações de versão
class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);
};
