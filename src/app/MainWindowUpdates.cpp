#include "MainWindow.h"
#include "core/Settings.h"
#include "gui/ServerTab.h"
#include "version.h"

#include <QApplication>
#include <QCryptographicHash>
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
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVersionNumber>
#include <QVBoxLayout>

namespace {
constexpr qint64 kMaxInstallerBytes = 200LL * 1024LL * 1024LL;

QString semanticVersion(const QString& text) {
    static const QRegularExpression re(QStringLiteral(R"((\d+\.\d+\.\d+))"));
    const QRegularExpressionMatch match = re.match(text);
    return match.hasMatch() ? match.captured(1) : QString();
}

bool trustedReleaseUrl(const QUrl& url) {
    if (url.scheme() != QLatin1String("https")) return false;
    const QString host = url.host().toLower();
    return host == QLatin1String("github.com")
        || host == QLatin1String("objects.githubusercontent.com")
        || host == QLatin1String("release-assets.githubusercontent.com")
        || host.endsWith(QLatin1String(".githubusercontent.com"));
}

bool verifyAuthenticode(const QString& path, QString* detail) {
#ifdef Q_OS_WIN
    QProcess process;
    const QString script = QStringLiteral(
        "$s=Get-AuthenticodeSignature -LiteralPath $args[0]; "
        "Write-Output ($s.Status.ToString()+'|'+$s.SignerCertificate.Subject); "
        "if($s.Status -eq 'Valid'){exit 0}else{exit 2}");
    process.start(QStringLiteral("powershell.exe"),
                  {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                   QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                   QStringLiteral("-Command"), script, path});
    if (!process.waitForStarted(5000) || !process.waitForFinished(30000)) {
        if (detail) *detail = QObject::tr("Não foi possível executar a validação Authenticode");
        return false;
    }
    const QString output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    if (detail) *detail = output;
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
#else
    Q_UNUSED(path);
    if (detail) *detail = QObject::tr("Atualização automática por EXE só é suportada no Windows");
    return false;
#endif
}
}

void MainWindow::checkUpdates(bool manual) {
    QNetworkAccessManager* nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/GroupHalla/Halla/releases/latest")));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("HallaUpdater/%1").arg(QString::fromUtf8(halla::kAppVersion)));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam, manual] {
        const QByteArray data = reply->readAll();
        const QNetworkReply::NetworkError networkError = reply->error();
        reply->deleteLater();
        nam->deleteLater();
        if (networkError != QNetworkReply::NoError) {
            if (manual) QMessageBox::warning(this, tr("Verificar atualizações"),
                                             tr("Erro ao conectar ao servidor de atualizações."));
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) return;
        const QJsonObject obj = doc.object();
        const QString latestVersion = semanticVersion(obj.value(QStringLiteral("tag_name")).toString());
        const QString currentVersion = semanticVersion(QString::fromUtf8(halla::kAppVersion));
        if (latestVersion.isEmpty() || currentVersion.isEmpty()) return;

        if (QVersionNumber::fromString(latestVersion) <= QVersionNumber::fromString(currentVersion)) {
            if (manual) QMessageBox::information(this, tr("Atualização"),
                                                  tr("Você já está usando a versão mais recente do Halla."));
            return;
        }

        const QStringList allowedNames = {
            QStringLiteral("Halla-Setup-WebRTC-%1.exe").arg(latestVersion),
            QStringLiteral("Halla-Setup-%1.exe").arg(latestVersion)
        };
        QString downloadUrl;
        QString checksumUrl;
        QString selectedName;
        const QJsonArray assets = obj.value(QStringLiteral("assets")).toArray();
        for (const QString& allowed : allowedNames) {
            for (const QJsonValue& value : assets) {
                const QJsonObject asset = value.toObject();
                if (asset.value(QStringLiteral("name")).toString() == allowed) {
                    selectedName = allowed;
                    downloadUrl = asset.value(QStringLiteral("browser_download_url")).toString();
                }
            }
            if (!downloadUrl.isEmpty()) break;
        }
        if (!selectedName.isEmpty()) {
            for (const QJsonValue& value : assets) {
                const QJsonObject asset = value.toObject();
                if (asset.value(QStringLiteral("name")).toString() == selectedName + QStringLiteral(".sha256")) {
                    checksumUrl = asset.value(QStringLiteral("browser_download_url")).toString();
                    break;
                }
            }
        }

        if (downloadUrl.isEmpty() || checksumUrl.isEmpty()
            || !trustedReleaseUrl(QUrl(downloadUrl)) || !trustedReleaseUrl(QUrl(checksumUrl))) {
            if (manual) QMessageBox::warning(this, tr("Verificar atualizações"),
                tr("A release %1 não contém um instalador e checksum com nomes confiáveis.").arg(latestVersion));
            return;
        }

        const int answer = QMessageBox::question(this, tr("Nova atualização disponível"),
            tr("Uma nova versão (%1) está disponível!\nO SHA-256 é obrigatório; a assinatura Authenticode será validada quando disponível.\nDeseja continuar?").arg(latestVersion),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes)
            downloadAndInstallUpdate(downloadUrl, checksumUrl, latestVersion);
    });
}

void MainWindow::downloadAndInstallUpdate(const QString& url, const QString& checksumUrl,
                                          const QString& version) {
    if (!trustedReleaseUrl(QUrl(url)) || !trustedReleaseUrl(QUrl(checksumUrl))) {
        QMessageBox::critical(this, tr("Atualização insegura"), tr("URL de atualização não confiável."));
        return;
    }

    QDialog* dlg = new QDialog(this);
    dlg->setWindowTitle(tr("Baixando atualização"));
    dlg->resize(390, 110);
    QVBoxLayout* lay = new QVBoxLayout(dlg);
    QLabel* label = new QLabel(tr("Validando Halla v%1...").arg(version), dlg);
    lay->addWidget(label);
    QProgressBar* bar = new QProgressBar(dlg);
    bar->setRange(0, 0);
    lay->addWidget(bar);
    dlg->show();

    QNetworkAccessManager* nam = new QNetworkAccessManager(dlg);
    QNetworkRequest checksumRequest{QUrl(checksumUrl)};
    checksumRequest.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("HallaUpdater/%1").arg(version));
    checksumRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* checksumReply = nam->get(checksumRequest);

    connect(checksumReply, &QNetworkReply::finished, dlg,
            [this, dlg, label, bar, nam, checksumReply, url, version] {
        const QByteArray checksumData = checksumReply->readAll();
        const auto checksumError = checksumReply->error();
        checksumReply->deleteLater();
        static const QRegularExpression hashRe(QStringLiteral(R"(\b([0-9a-fA-F]{64})\b)"));
        const QRegularExpressionMatch match = hashRe.match(QString::fromLatin1(checksumData.left(4096)));
        if (checksumError != QNetworkReply::NoError || !match.hasMatch()) {
            QMessageBox::critical(this, tr("Atualização insegura"), tr("Checksum SHA-256 ausente ou inválido."));
            dlg->close();
            return;
        }
        const QByteArray expectedHash = match.captured(1).toLatin1().toLower();
        label->setText(tr("Baixando Halla v%1...").arg(version));
        bar->setRange(0, 100);

        QNetworkRequest request{QUrl(url)};
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("HallaUpdater/%1").arg(version));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = nam->get(request);
        connect(reply, &QNetworkReply::downloadProgress, dlg, [bar, reply](qint64 received, qint64 total) {
            if (received > kMaxInstallerBytes || total > kMaxInstallerBytes) {
                reply->abort();
                return;
            }
            if (total > 0) bar->setValue(int((received * 100) / total));
        });
        connect(reply, &QNetworkReply::finished, dlg,
                [this, reply, dlg, version, expectedHash] {
            const QByteArray installer = reply->readAll();
            const auto error = reply->error();
            reply->deleteLater();
            if (error != QNetworkReply::NoError || installer.isEmpty()
                || installer.size() > kMaxInstallerBytes) {
                QMessageBox::critical(this, tr("Erro de download"), tr("Download inválido ou acima de 200 MiB."));
                dlg->close();
                return;
            }
            const QByteArray actualHash = QCryptographicHash::hash(installer, QCryptographicHash::Sha256).toHex();
            if (actualHash != expectedHash) {
                QMessageBox::critical(this, tr("Atualização insegura"), tr("O SHA-256 do instalador não confere."));
                dlg->close();
                return;
            }

            const QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
            const QString installerPath = tempDir + QStringLiteral("/Halla-Setup-") + version + QStringLiteral(".exe");
            QFile file(installerPath);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                || file.write(installer) != installer.size()) {
                QMessageBox::critical(this, tr("Erro"), tr("Não foi possível salvar o instalador."));
                dlg->close();
                return;
            }
            file.close();

            QString signatureDetail;
            const bool signatureValid = verifyAuthenticode(installerPath, &signatureDetail);
            if (!signatureValid) {
                const int unsignedAnswer = QMessageBox::warning(this, tr("Instalador sem certificado"),
                    tr("O SHA-256 publicado foi validado, mas este instalador ainda não possui uma assinatura Authenticode confiável.\n"
                       "O Windows poderá mostrar ‘Editor desconhecido’.\n\nDetalhes: %1\n\nDeseja executar mesmo assim?")
                        .arg(signatureDetail),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (unsignedAnswer != QMessageBox::Yes) {
                    QFile::remove(installerPath);
                    dlg->close();
                    return;
                }
            }

            QMessageBox::information(this, tr("Download concluído"),
                signatureValid
                    ? tr("SHA-256 e assinatura Authenticode validados. O instalador será executado agora.")
                    : tr("SHA-256 validado. O instalador sem certificado será executado por sua confirmação."));
            S::set("app/forceQuit", true);
            QProcess::startDetached(installerPath, {});
            qApp->quit();
            dlg->close();
        });
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