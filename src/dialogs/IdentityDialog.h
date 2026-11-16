#pragma once

#include <QDialog>
#include <QTableWidget>

// "Identities" window -- manages the local identities (unique ID), like Halla's
// identities window.
class IdentityDialog : public QDialog {
    Q_OBJECT
public:
    explicit IdentityDialog(QWidget* parent = nullptr);

    static QString generateUniqueId();
    static QByteArray publicKeyForUid(const QString& uid);
    static QByteArray signNonce(const QString& uid, const QByteArray& nonce);
    // v6 E2EE: persistent X25519 pair of the identity + signed binding. Ensures
    // existence (generates on the first call) and returns the material.
    static bool ensureDhKeyPair(const QString& uid);
    static QByteArray dhPrivateKeyForUid(const QString& uid);
    static QByteArray dhPublicKeyForUid(const QString& uid);
    // Deterministic Ed25519 signature binding X25519 to the identity -- recomputed
    // on every use, never persisted.
    static QByteArray dhSignatureForUid(const QString& uid);
    static QList<QStringList> loadAll();     // [default, name, phonetic, unique id]
    static void saveAll(const QList<QStringList>& rows);
    static QString defaultNickname();

private:
    void reload();
    int selectedRow() const;

    QTableWidget* m_table;
    class QToolButton* m_defaultBtn;
    class QLabel* m_note = nullptr;
};
