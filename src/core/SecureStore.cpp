#include "SecureStore.h"

#include <QEventLoop>
#include <qtkeychain/keychain.h>

namespace {
constexpr auto kService = "Halla";

template <typename Job>
bool runJob(Job& job, QString* error) {
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
    if (job.error() == QKeychain::NoError) return true;
    if (error) *error = job.errorString();
    return false;
}
}

namespace SecureStore {

bool write(const QString& key, const QByteArray& value, QString* error) {
    QKeychain::WritePasswordJob job(QString::fromLatin1(kService));
    job.setAutoDelete(false);
    job.setKey(key);
    job.setTextData(QString::fromLatin1(value.toBase64()));
    return runJob(job, error);
}

QByteArray read(const QString& key, QString* error) {
    QKeychain::ReadPasswordJob job(QString::fromLatin1(kService));
    job.setAutoDelete(false);
    job.setKey(key);
    if (!runJob(job, error)) return {};
    return QByteArray::fromBase64(job.textData().toLatin1());
}

bool remove(const QString& key, QString* error) {
    QKeychain::DeletePasswordJob job(QString::fromLatin1(kService));
    job.setAutoDelete(false);
    job.setKey(key);
    if (runJob(job, error)) return true;
    // Remover chave ausente é idempotente.
    if (job.error() == QKeychain::EntryNotFound) return true;
    return false;
}

} // namespace SecureStore
