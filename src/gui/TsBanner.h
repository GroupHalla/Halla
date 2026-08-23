#pragma once

#include <QWidget>
#include <QString>

// Banner roxo arredondado usado nos diálogos do Halla.
class TsBanner : public QWidget {
    Q_OBJECT
public:
    explicit TsBanner(const QString& title, const QString& subtitle = QString(),
                      const QPixmap& icon = QPixmap(), QWidget* parent = nullptr);

    QSize sizeHint() const override { return QSize(420, 68); }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QString m_title;
    QString m_subtitle;
    QPixmap m_icon;
};
