#pragma once

#include <QDialog>
#include <QTableWidget>

// Janela "Identidades" — gerencia as identidades locais (ID único),
// como a janela de identidades do Halla.
class IdentityDialog : public QDialog {
    Q_OBJECT
public:
    explicit IdentityDialog(QWidget* parent = nullptr);

    static QString generateUniqueId();
    static QByteArray publicKeyForUid(const QString& uid);
    static QByteArray signNonce(const QString& uid, const QByteArray& nonce);
    // v6 E2EE: par X25519 persistente da identidade + binding assinado.
    // Garante existência (gera na primeira chamada) e devolve o material.
    static bool ensureDhKeyPair(const QString& uid);
    static QByteArray dhPrivateKeyForUid(const QString& uid);
    static QByteArray dhPublicKeyForUid(const QString& uid);
    // Assinatura Ed25519 determinística que liga a X25519 à identidade —
    // recalculada a cada uso, nunca persistida.
    static QByteArray dhSignatureForUid(const QString& uid);
    static QList<QStringList> loadAll();     // [default, nome, fonético, id único]
    static void saveAll(const QList<QStringList>& rows);
    static QString defaultNickname();

private:
    void reload();
    int selectedRow() const;

    QTableWidget* m_table;
    class QToolButton* m_defaultBtn;
    class QLabel* m_note = nullptr;
};
