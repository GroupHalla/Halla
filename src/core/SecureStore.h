#pragma once

#include <QByteArray>
#include <QString>

// OS-native secret storage backed by QtKeychain: Credential Manager
// (Windows), Secret Service/KWallet (Linux), and Keychain (macOS).
namespace SecureStore {

bool write(const QString& key, const QByteArray& value, QString* error = nullptr);
QByteArray read(const QString& key, QString* error = nullptr);
bool remove(const QString& key, QString* error = nullptr);

} // namespace SecureStore
