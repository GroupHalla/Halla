#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QTimer>
#include <QPixmap>
#include <QDir>
#include "app/MainWindow.h"
#include "gui/Icons.h"
#include "core/Settings.h"
#include "dialogs/ConnectDialog.h"
#include "dialogs/OptionsDialog.h"
#include "dialogs/ChannelDialog.h"
#include "version.h"

// Modo especial para capturas de tela automatizadas (usado em desenvolvimento):
//   Halla --shot <caminho.png> <janela>   (janela: main | demo | connect | options | channel)
static int takeShot(QApplication& app, const QString& path, const QString& what) {
    QTimer timer;
    timer.setSingleShot(true);

    MainWindow w;

    auto grab = [&]() {
        if (what == "demo") w.loadDemoState();
        w.resize(1180, 760);
        w.show();
        app.processEvents();
        QTimer::singleShot(120, &w, [&w, path, &app] {
            w.grab().save(path);
            app.quit();
        });
    };

    if (what == "connect") {
        ConnectDialog* dlg = new ConnectDialog(&w);
        dlg->setNickname(QStringLiteral("HallaUser"));
        dlg->setAddress(QStringLiteral("meuservidor.exemplo.com"));
        w.resize(900, 600);
        w.show();
        dlg->show();
        app.processEvents();
        QTimer::singleShot(200, &w, [dlg, path, &app] {
            dlg->grab().save(path);
            app.quit();
        });
    } else if (what == "options") {
        OptionsDialog* dlg = new OptionsDialog(&w);
        w.resize(900, 600);
        w.show();
        dlg->show();
        app.processEvents();
        QTimer::singleShot(200, &w, [dlg, path, &app] {
            dlg->grab().save(path);
            app.quit();
        });
    } else if (what == "channel") {
        w.loadDemoState();
        ServerData* d = nullptr;
        // usa o diálogo de canal com dados da aba demo
        ChannelDialog* dlg = new ChannelDialog(QObject::tr("Criar canal"), d, &w);
        w.show();
        dlg->show();
        app.processEvents();
        QTimer::singleShot(200, &w, [dlg, path, &app] {
            dlg->grab().save(path);
            app.quit();
        });
    } else {
        grab();
    }

    return app.exec();
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Halla"));
    QApplication::setOrganizationName(QStringLiteral("Halla"));
    QApplication::setApplicationDisplayName(QStringLiteral("Halla"));
    QApplication::setApplicationVersion(QString::fromUtf8(halla::kAppVersion));
    QApplication::setWindowIcon(QIcon(HIcons::appIcon(64)));

    // fonte padrão estilo Segoe/8.25pt quando disponível
    {
        QFont f = app.font();
        const QStringList preferred = { "Segoe UI", "Noto Sans", "DejaVu Sans",
                                        "Liberation Sans", "Arial" };
        for (const QString& fam : preferred) {
            if (QFontDatabase::families().contains(fam)) { f.setFamily(fam); break; }
        }
        f.setPointSize(qMax(8, S::num("design/fontSize", 9)));
        app.setFont(f);
    }

    // modo de captura de tela (desenvolvimento)
    const QStringList args = app.arguments();
    const int shotIdx = args.indexOf(QStringLiteral("--shot"));
    if (shotIdx >= 0 && shotIdx + 2 < args.size()) {
        return takeShot(app, args.at(shotIdx + 1), args.at(shotIdx + 2));
    }

    MainWindow w;
    w.show();
    return app.exec();
}
