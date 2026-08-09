#include "MainWindow.h"
#include "core/Settings.h"
#include "gui/ServerTab.h"
#include "version.h"

#include <QApplication>
#include <QCursor>
#include <QDialog>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressBar>
#include <QStandardPaths>
#include <QVBoxLayout>

// Atualizações, menu de notificações e salvamento/restauração de sessão.
// Extraído de MainWindow.cpp para reduzir o arquivo monolítico.

void MainWindow::checkUpdates(bool manual) {
    QNetworkAccessManager* nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/GroupHalla/Halla/releases/latest")));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("HallaUpdater"));
    
    QNetworkReply* reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam, manual] {
        reply->deleteLater();
        nam->deleteLater();
        
        if (reply->error() != QNetworkReply::NoError) {
            if (manual) {
                QMessageBox::warning(this, tr("Verificar atualizações"),
                                     tr("Erro ao conectar ao servidor de atualizações."));
            }
            return;
        }
        
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) return;
        
        QJsonObject obj = doc.object();
        QString latestVersion = obj["tag_name"].toString();
        if (latestVersion.startsWith('v')) latestVersion.remove(0, 1);
        
        QString currentVersion = QString::fromUtf8(halla::kAppVersion);
        
        if (latestVersion != currentVersion && !latestVersion.isEmpty()) {
            QString downloadUrl;
            QJsonArray assets = obj["assets"].toArray();
            for (const QJsonValue& val : assets) {
                QJsonObject assetObj = val.toObject();
                QString assetName = assetObj["name"].toString();
                if (assetName.contains("Setup") && assetName.endsWith(".exe")) {
                    downloadUrl = assetObj["browser_download_url"].toString();
                    break;
                }
            }
            
            if (downloadUrl.isEmpty() && !assets.isEmpty()) {
                downloadUrl = assets[0].toObject()["browser_download_url"].toString();
            }
            
            if (!downloadUrl.isEmpty()) {
                int ret = QMessageBox::question(this, tr("Nova atualização disponível"),
                    tr("Uma nova versão (%1) está disponível!\nDeseja baixar e instalar agora?").arg(latestVersion),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (ret == QMessageBox::Yes) {
                    downloadAndInstallUpdate(downloadUrl, latestVersion);
                }
            }
        } else {
            if (manual) {
                QMessageBox::information(this, tr("Atualização"),
                                         tr("Você já está usando a versão mais recente do Halla."));
            }
        }
    });
}

void MainWindow::downloadAndInstallUpdate(const QString& url, const QString& version) {
    QDialog* dlg = new QDialog(this);
    dlg->setWindowTitle(tr("Baixando atualização"));
    dlg->resize(350, 100);
    
    QVBoxLayout* lay = new QVBoxLayout(dlg);
    QLabel* label = new QLabel(tr("Baixando Halla v%1...").arg(version), dlg);
    lay->addWidget(label);
    
    QProgressBar* bar = new QProgressBar(dlg);
    bar->setRange(0, 100);
    bar->setValue(0);
    lay->addWidget(bar);
    
    dlg->show();
    
    QNetworkAccessManager* nam = new QNetworkAccessManager(dlg);
    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("HallaUpdater"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    
    QNetworkReply* reply = nam->get(req);
    
    connect(reply, &QNetworkReply::downloadProgress, dlg, [bar](qint64 bytesReceived, qint64 bytesTotal) {
        if (bytesTotal > 0) {
            bar->setValue(int((bytesReceived * 100) / bytesTotal));
        }
    });
    
    connect(reply, &QNetworkReply::finished, dlg, [this, reply, dlg, version] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QMessageBox::critical(this, tr("Erro de download"), tr("Não foi possível baixar o instalador da atualização."));
            dlg->close();
            return;
        }
        
        QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        QString installerPath = tempDir + QStringLiteral("/Halla-Setup-") + version + QStringLiteral(".exe");
        
        QFile file(installerPath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(reply->readAll());
            file.close();
            
            QMessageBox::information(this, tr("Download concluído"),
                tr("O download foi concluído com sucesso. O instalador será executado agora."));
                
            S::set("app/forceQuit", true); // Bypassa todos os diálogos de confirmação de saída!
            QProcess::startDetached(installerPath, QStringList());
            qApp->quit();
        } else {
            QMessageBox::critical(this, tr("Erro"), tr("Não foi possível salvar o arquivo de atualização no diretório temporário."));
        }
        dlg->close();
    });
}

void MainWindow::showNotifications() {
    QMenu menu(this);
    QAction* header = menu.addAction(tr("Notificações"));
    header->setEnabled(false);
    menu.addSeparator();
    QAction* none = menu.addAction(tr("Nenhuma notificação nova"));
    none->setEnabled(false);
    menu.exec(QCursor::pos());
}

void MainWindow::saveSession() {
    QJsonArray arr;
    for (int i = 0; i < m_tabs->count(); ++i) {
        ServerTab* t = qobject_cast<ServerTab*>(m_tabs->widget(i));
        if (!t) continue;
        QJsonObject o;
        o["addr"] = t->data().name;
        o["port"] = 9987;
        o["nick"] = t->data().users[t->data().selfId].name;
        arr << o;
    }
    S::set("session/tabs",
           QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}