#pragma once

#include <QIcon>
#include <QPixmap>
#include <QColor>

// Ícones do Halla — todos renderizados em código (QPainter) no estilo visual
// clássico do Halla, sem depender de arquivos externos.
namespace HIcons {

// cores da identidade visual
inline QColor blue()        { return QColor("#7C3AED"); }
inline QColor blueDark()    { return QColor("#25104F"); }
inline QColor navyMid()     { return QColor("#4B1C9B"); }
inline QColor green()       { return QColor("#3CA55C"); }
inline QColor orange()      { return QColor("#E67E22"); }
inline QColor red()         { return QColor("#D9534F"); }
inline QColor gold()        { return QColor("#E8B23C"); }
inline QColor grayLine()    { return QColor("#8A939B"); }

QPixmap appIcon(int size = 64);
QPixmap banner(int w = 560, int h = 58);
QPixmap waveMark(int size = 48, const QColor& color = QColor(255, 255, 255));

// conexão / toolbar
QIcon connectPlug();
QIcon disconnectPlug();
QIcon bookmarkStar();
QIcon optionsGear();
QIcon logPage();
QIcon away(bool on);
QIcon muteMic(bool muted);
QIcon muteSpeaker(bool muted);
QIcon bell();

// árvore do servidor
QIcon server();
QIcon channel(bool hasPassword, bool moderated, bool isDefault, bool full);
QIcon user(bool talking, bool away, int size = 20, bool whispering = false);
QPixmap userStatusMinis(bool inputMuted, bool outputMuted, bool away,
                        bool recording, bool commander, bool op = false);

// menus / diálogos
QIcon identity();
QIcon contacts();
QIcon transfer();
QIcon key();
QIcon groups();
QIcon captureMic();
QIcon playbackSpeaker();
QIcon hotkeys();
QIcon design();
QIcon notifyBell();
QIcon security();
QIcon addons();
QIcon application();
QIcon info();
QIcon fileNew();
QIcon editPencil();
QIcon trash();
QIcon check();
QIcon record(bool on);

} // namespace HIcons
