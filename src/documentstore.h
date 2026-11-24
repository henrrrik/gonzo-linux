#pragma once

#include <QByteArray>
#include <QLockFile>
#include <QString>
#include <QStringList>
#include <memory>
#include <optional>

struct ReadFileResult {
    bool ok = false;
    bool oversized = false;
    bool notFound = false;
    QString resolvedPath;
    QString text;
    QByteArray sha256;
    qint64 utf8Bytes = 0;
    QString error;
};

struct WriteFileResult {
    bool ok = false;
    bool conflict = false;
    QString resolvedPath;
    QByteArray sha256;
    QString error;
};

struct RecoveryDraft {
    QString draftId;
    QString originalPath;
    QString text;
    QByteArray recordedBaseline;
    bool hasConflictBaseline = false;
    QByteArray conflictBaseline;
};

// Safe, bounded local file I/O, SHA-256 external-change conflict protection,
// atomic writes, crash recovery drafts, and recent document history.
class DocumentStore {
public:
    static constexpr qint64 kMaxFileBytes = 16 * 1024 * 1024;
    static constexpr int kMaxRecents = 12;

    explicit DocumentStore(const QString &stateDirectory = {});
    ~DocumentStore();

    static QString defaultStateDirectory();
    static QString resolveTargetPath(const QString &path);
    static QByteArray sha256(const QByteArray &data);
    static QByteArray unmatchableBaseline();

    static ReadFileResult readFile(const QString &path);
    static WriteFileResult writeFile(const QString &path, const QByteArray &utf8Bytes,
                                     const QByteArray &expectedBaseline = {},
                                     bool checkConflict = false);
    static bool diskMatchesBaseline(const QString &path, const QByteArray &baseline,
                                    bool *deleted = nullptr);

    // Per-window recovery slot locked while this window is open.
    bool writeRecoveryDraft(const QString &originalPath, const QString &text,
                            const QByteArray &recordedBaseline);
    void clearRecoveryDraft();
    std::optional<RecoveryDraft> takeRecoverableDraft();

    // Recent documents list persisted in stateDirectory/recents.json.
    QStringList recentFiles() const;
    void addRecentFile(const QString &path);
    void clearRecentFiles();

private:
    QString recoveryDir() const;
    QString recentsPath() const;
    bool ensureDraftLock();

    QString m_stateDir;
    QString m_draftId;
    std::unique_ptr<QLockFile> m_draftLock;
};
