#include "documentstore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct RawReadResult {
    bool ok = false;
    bool oversized = false;
    bool notFound = false;
    QString resolvedPath;
    QByteArray bytes;
    QString error;
};

RawReadResult readBoundedBytes(const QString &inputPath) {
    RawReadResult res;
    const QString resolved = DocumentStore::resolveTargetPath(inputPath);
    res.resolvedPath = resolved;
    const QByteArray native = QFile::encodeName(resolved);

    // Follow a symlink the user chose above, then open without following another
    // link and without blocking, so a FIFO or device can never hang the read.
    const int fd = ::open(native.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        if (err == ENOENT) {
            res.notFound = true;
            res.error = QStringLiteral("File not found.");
        } else if (err == EACCES || err == EPERM) {
            res.error = QStringLiteral("Permission denied.");
        } else {
            res.error = QStringLiteral("Gonzo opens regular local files.");
        }
        return res;
    }

    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(fd);
        res.error = QStringLiteral("Gonzo opens regular local files.");
        return res;
    }

    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    QByteArray data;
    if (info.st_size > 0 && info.st_size <= DocumentStore::kMaxFileBytes)
        data.reserve(static_cast<qsizetype>(info.st_size));

    char buffer[256 * 1024];
    for (;;) {
        const qint64 remaining = DocumentStore::kMaxFileBytes + 1 - data.size();
        const size_t toRead = static_cast<size_t>(std::min<qint64>(sizeof(buffer), remaining));
        const ssize_t n = ::read(fd, buffer, toRead);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ::close(fd);
            res.error = QStringLiteral("Could not read file: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
            return res;
        }
        if (n == 0)
            break;
        data.append(buffer, static_cast<qsizetype>(n));
        if (data.size() > DocumentStore::kMaxFileBytes) {
            ::close(fd);
            res.oversized = true;
            res.error = QStringLiteral("This document exceeds Gonzo's 16 MB limit.");
            return res;
        }
    }

    ::close(fd);
    res.ok = true;
    res.bytes = std::move(data);
    return res;
}

} // namespace

DocumentStore::DocumentStore(const QString &stateDirectory)
    : m_stateDir(stateDirectory.isEmpty() ? defaultStateDirectory() : stateDirectory) {}

DocumentStore::~DocumentStore() {
    if (m_draftLock)
        m_draftLock->unlock();
}

QString DocumentStore::defaultStateDirectory() {
    const QString xdgState = qEnvironmentVariable("XDG_STATE_HOME");
    if (!xdgState.isEmpty())
        return xdgState + QStringLiteral("/gonzo-writer");
    return QDir::homePath() + QStringLiteral("/.local/state/gonzo-writer");
}

QString DocumentStore::resolveTargetPath(const QString &path) {
    if (path.isEmpty())
        return path;
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty())
        return canonical;
    // For a file that doesn't exist yet, resolve its parent directory if possible.
    const QString parentCanonical = QFileInfo(info.absolutePath()).canonicalFilePath();
    if (!parentCanonical.isEmpty())
        return parentCanonical + QLatin1Char('/') + info.fileName();
    return info.absoluteFilePath();
}

QByteArray DocumentStore::sha256(const QByteArray &data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QByteArray DocumentStore::unmatchableBaseline() {
    // 31 zero bytes never equals any 32-byte SHA-256 digest.
    return QByteArray(31, '\0');
}

ReadFileResult DocumentStore::readFile(const QString &path) {
    ReadFileResult out;
    const RawReadResult raw = readBoundedBytes(path);
    out.resolvedPath = raw.resolvedPath;
    out.oversized = raw.oversized;
    out.notFound = raw.notFound;
    if (!raw.ok) {
        out.error = raw.error;
        return out;
    }

    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = decoder.decode(raw.bytes);
    if (decoder.hasError()) {
        out.error = QStringLiteral("Gonzo opens UTF-8 Markdown and plain text files.");
        return out;
    }

    if (!text.isEmpty() && text.at(0).unicode() == 0xFEFF)
        text.remove(0, 1);

    out.ok = true;
    out.text = std::move(text);
    out.sha256 = sha256(raw.bytes);
    out.utf8Bytes = out.text.toUtf8().size();
    return out;
}

WriteFileResult DocumentStore::writeFile(const QString &path, const QByteArray &utf8Bytes,
                                         const QByteArray &expectedBaseline, bool checkConflict) {
    WriteFileResult out;
    if (utf8Bytes.size() > kMaxFileBytes) {
        out.error = QStringLiteral("This document exceeds Gonzo's 16 MB limit.");
        return out;
    }

    const QString resolved = resolveTargetPath(path);
    out.resolvedPath = resolved;

    if (checkConflict && !expectedBaseline.isEmpty()) {
        const RawReadResult current = readBoundedBytes(resolved);
        if (!current.ok) {
            if (!current.notFound) {
                out.conflict = true;
                out.error = QStringLiteral("The file changed outside Gonzo. Your edits have not overwritten it.");
                return out;
            }
            // If the original file was deleted while editing, require explicit Save As or Save confirmation.
            out.conflict = true;
            out.error = QStringLiteral("The file was moved or deleted outside Gonzo.");
            return out;
        }
        if (sha256(current.bytes) != expectedBaseline) {
            out.conflict = true;
            out.error = QStringLiteral("The file changed outside Gonzo. Your edits have not overwritten it.");
            return out;
        }
    }

    QSaveFile file(resolved);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        out.error = QStringLiteral("Could not save file: %1").arg(file.errorString());
        return out;
    }
    if (file.write(utf8Bytes) != utf8Bytes.size() || !file.commit()) {
        out.error = QStringLiteral("Could not save file: %1").arg(file.errorString());
        return out;
    }

    out.ok = true;
    out.sha256 = sha256(utf8Bytes);
    return out;
}

bool DocumentStore::diskMatchesBaseline(const QString &path, const QByteArray &baseline, bool *deleted) {
    if (deleted)
        *deleted = false;
    if (path.isEmpty() || baseline.isEmpty())
        return true;
    const RawReadResult current = readBoundedBytes(path);
    if (!current.ok) {
        if (deleted)
            *deleted = current.notFound;
        return false;
    }
    return sha256(current.bytes) == baseline;
}

QString DocumentStore::recoveryDir() const {
    return m_stateDir + QStringLiteral("/recovery");
}

QString DocumentStore::recentsPath() const {
    return m_stateDir + QStringLiteral("/recents.json");
}

bool DocumentStore::ensureDraftLock() {
    if (m_draftLock && m_draftLock->isLocked())
        return true;
    QDir().mkpath(recoveryDir());
    if (m_draftId.isEmpty())
        m_draftId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_draftLock = std::make_unique<QLockFile>(recoveryDir() + QLatin1Char('/') + m_draftId + QStringLiteral(".lock"));
    m_draftLock->setStaleLockTime(0);
    return m_draftLock->tryLock(0);
}

bool DocumentStore::writeRecoveryDraft(const QString &originalPath, const QString &text,
                                       const QByteArray &recordedBaseline) {
    if (!ensureDraftLock())
        return false;

    const QString base = recoveryDir() + QLatin1Char('/') + m_draftId;
    QSaveFile draftFile(base + QStringLiteral(".md"));
    const QByteArray utf8 = text.toUtf8();
    if (!draftFile.open(QIODevice::WriteOnly) || draftFile.write(utf8) != utf8.size() || !draftFile.commit())
        return false;

    QJsonObject meta;
    meta.insert(QStringLiteral("originalPath"), originalPath);
    if (!recordedBaseline.isEmpty())
        meta.insert(QStringLiteral("baselineSha256"), QString::fromLatin1(recordedBaseline.toHex()));
    meta.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    QSaveFile metaFile(base + QStringLiteral(".json"));
    const QByteArray json = QJsonDocument(meta).toJson(QJsonDocument::Compact);
    if (!metaFile.open(QIODevice::WriteOnly) || metaFile.write(json) != json.size() || !metaFile.commit())
        return false;
    return true;
}

void DocumentStore::clearRecoveryDraft() {
    if (m_draftId.isEmpty())
        return;
    const QString base = recoveryDir() + QLatin1Char('/') + m_draftId;
    QFile::remove(base + QStringLiteral(".md"));
    QFile::remove(base + QStringLiteral(".json"));
    if (m_draftLock) {
        m_draftLock->unlock();
        m_draftLock.reset();
    }
    QFile::remove(base + QStringLiteral(".lock"));
    m_draftId.clear();
}

std::optional<RecoveryDraft> DocumentStore::takeRecoverableDraft() {
    const QDir dir(recoveryDir());
    if (!dir.exists())
        return std::nullopt;

    const QFileInfoList entries = dir.entryInfoList({QStringLiteral("*.md")}, QDir::Files, QDir::Time);
    for (const QFileInfo &entry : entries) {
        const QString id = entry.completeBaseName();
        if (id == m_draftId)
            continue;

        auto lock = std::make_unique<QLockFile>(dir.filePath(id + QStringLiteral(".lock")));
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0))
            continue;

        const ReadFileResult draftRead = readFile(entry.absoluteFilePath());
        if (!draftRead.ok) {
            // Only remove drafts that are genuinely missing or empty; preserve
            // unreadable drafts (e.g. transient EACCES/EMFILE/EIO) so user data
            // is never destroyed.
            const QFileInfo draftInfo(entry.absoluteFilePath());
            if (draftRead.notFound || (draftInfo.exists() && draftInfo.size() == 0)) {
                QFile::remove(entry.absoluteFilePath());
                QFile::remove(dir.filePath(id + QStringLiteral(".json")));
                lock->unlock();
                QFile::remove(dir.filePath(id + QStringLiteral(".lock")));
            } else {
                lock->unlock();
            }
            continue;
        }

        RecoveryDraft draft;
        draft.draftId = id;
        draft.text = draftRead.text;

        QFile metaFile(dir.filePath(id + QStringLiteral(".json")));
        if (metaFile.open(QIODevice::ReadOnly)) {
            const QJsonObject meta = QJsonDocument::fromJson(metaFile.readAll()).object();
            draft.originalPath = meta.value(QStringLiteral("originalPath")).toString();
            const QString hex = meta.value(QStringLiteral("baselineSha256")).toString();
            if (!hex.isEmpty())
                draft.recordedBaseline = QByteArray::fromHex(hex.toLatin1());
        }

        // Preserve conflict protection against changes made while Gonzo was closed.
        if (!draft.originalPath.isEmpty()) {
            const RawReadResult original = readBoundedBytes(draft.originalPath);
            if (original.ok) {
                draft.hasConflictBaseline = true;
                draft.conflictBaseline = !draft.recordedBaseline.isEmpty()
                    ? draft.recordedBaseline
                    : sha256(original.bytes);
            } else if (original.notFound) {
                draft.hasConflictBaseline = false;
            } else {
                // Original exists but is unreadable or oversized: never overwrite it.
                draft.hasConflictBaseline = true;
                draft.conflictBaseline = unmatchableBaseline();
            }
        }

        // Adopt this draft's slot & lock so subsequent autosave/recovery keeps the same slot.
        if (m_draftLock)
            m_draftLock->unlock();
        m_draftId = id;
        m_draftLock = std::move(lock);
        return draft;
    }
    return std::nullopt;
}

QStringList DocumentStore::recentFiles() const {
    QFile file(recentsPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonArray arr = QJsonDocument::fromJson(file.readAll()).array();
    QStringList out;
    for (const QJsonValue &val : arr) {
        const QString p = val.toString();
        if (!p.isEmpty() && !out.contains(p))
            out.append(p);
        if (out.size() >= kMaxRecents)
            break;
    }
    return out;
}

void DocumentStore::addRecentFile(const QString &path) {
    const QString resolved = resolveTargetPath(path);
    if (resolved.isEmpty())
        return;
    QStringList list = recentFiles();
    list.removeAll(resolved);
    list.prepend(resolved);
    while (list.size() > kMaxRecents)
        list.removeLast();

    QDir().mkpath(m_stateDir);
    QJsonArray arr;
    for (const QString &item : list)
        arr.append(item);
    QSaveFile file(recentsPath());
    const QByteArray json = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    if (file.open(QIODevice::WriteOnly) && file.write(json) == json.size())
        file.commit();
}

void DocumentStore::clearRecentFiles() {
    QFile::remove(recentsPath());
}
