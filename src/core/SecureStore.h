#pragma once

#include <QByteArray>
#include <QString>

// Armazenamento de segredos no cofre nativo do sistema operacional por QtKeychain:
// Credential Manager (Windows), Secret Service/KWallet (Linux) e Keychain (macOS).
namespace SecureStore {

bool write(const QString& key, const QByteArray& value, QString* error = nullptr);
QByteArray read(const QString& key, QString* error = nullptr);
bool remove(const QString& key, QString* error = nullptr);

} // namespace SecureStore
