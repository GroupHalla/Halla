#include "HotkeyEdit.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QKeySequence>

HotkeyEdit::HotkeyEdit(QWidget* parent) : QLineEdit(parent) {
    setReadOnly(true);
    setPlaceholderText(tr("Clique e pressione uma tecla ou botão do mouse"));
    setClearButtonEnabled(false);
}

void HotkeyEdit::setSpec(const QString& spec) {
    m_spec = spec;
    setText(spec);
}

void HotkeyEdit::keyPressEvent(QKeyEvent* e) {
    const int key = e->key();
    if (key == Qt::Key_Escape || key == Qt::Key_Backspace || key == Qt::Key_Delete) {
        setSpec(QString());
        emit specChanged(m_spec);
        e->accept();
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
    setSpec(QKeySequence(combined).toString());
    emit specChanged(m_spec);
    e->accept();
}

void HotkeyEdit::mousePressEvent(QMouseEvent* e) {
    QString name;
    switch (e->button()) {
    case Qt::XButton1:    name = QString::fromLatin1(kMouse4); break;
    case Qt::XButton2:    name = QString::fromLatin1(kMouse5); break;
    case Qt::MiddleButton: name = QString::fromLatin1(kMouseMiddle); break;
    default: break;
    }
    if (!name.isEmpty()) {
        setSpec(name);
        emit specChanged(m_spec);
        e->accept();
        return;
    }
    QLineEdit::mousePressEvent(e);
}
