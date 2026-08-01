#pragma once

#include <QWidget>
#include <QString>

// Faixa azul-marinho arredondada no topo das janelas de diálogo — assinatura
// visual dos diálogos do TeamSpeak 3 (Opções, Criar Canal, Favoritos, etc.)
class TsBanner : public QWidget {
    Q_OBJECT
public:
    explicit TsBanner(const QString& title, const QString& subtitle = QString(),
                      const QPixmap& icon = QPixmap(), QWidget* parent = nullptr);

    QSize sizeHint() const override { return QSize(300, 56); }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QString m_title;
    QString m_subtitle;
    QPixmap m_icon;
};
