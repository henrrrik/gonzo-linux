#pragma once

#include "documentstore.h"
#include "markdown.h"
#include "spellchecker.h"
#include "textmetrics.h"

#include <QFileSystemWatcher>
#include <QFont>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <memory>

class FilePicker;
class MarkdownHighlighter;
class Printer;
class QQuickTextDocument;
class QTextDocument;
class Theme;

// Owns Gonzo's document state, editing commands, incremental word count,
// safe file I/O, autosave/recovery, find & replace, spell checking, recents,
// and printing. Exposed to QML as `backend`.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString filePath READ filePath NOTIFY documentChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY documentChanged)
    Q_PROPERTY(QString displayTitle READ displayTitle NOTIFY documentChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(int wordCount READ wordCount NOTIFY wordCountChanged)
    Q_PROPERTY(qint64 utf8Bytes READ utf8Bytes NOTIFY wordCountChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY statusChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily NOTIFY typographyChanged)
    Q_PROPERTY(qreal fontSize READ fontSize NOTIFY typographyChanged)
    Q_PROPERTY(qreal columnWidth READ columnWidth NOTIFY typographyChanged)
    Q_PROPERTY(QStringList recentFiles READ recentFiles NOTIFY recentFilesChanged)
    Q_PROPERTY(bool spellCheckAvailable READ spellCheckAvailable NOTIFY spellCheckChanged)
    Q_PROPERTY(bool spellCheckEnabled READ spellCheckEnabled NOTIFY spellCheckChanged)
    Q_PROPERTY(bool conflictPending READ conflictPending NOTIFY conflictChanged)
    Q_PROPERTY(bool canRevert READ canRevert NOTIFY documentChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery NOTIFY searchChanged)
    Q_PROPERTY(int searchMatchCount READ searchMatchCount NOTIFY searchChanged)
    Q_PROPERTY(int currentSearchIndex READ currentSearchIndex NOTIFY searchChanged)

public:
    static constexpr qreal kDefaultFontSize = 13.0;
    static constexpr qreal kMinFontSize = 12.0;
    static constexpr qreal kMaxFontSize = 32.0;

    explicit Backend(Theme *theme = nullptr,
                     FilePicker *filePicker = nullptr,
                     Printer *printer = nullptr,
                     const QString &stateDirectory = {},
                     const QString &configDirectory = {},
                     const QStringList &spellSearchDirs = {},
                     QObject *parent = nullptr);
    ~Backend() override;

    QString filePath() const { return m_filePath; }
    QString fileName() const;
    QString displayTitle() const;
    bool modified() const { return m_modified; }
    int wordCount() const { return m_metrics.wordCount(); }
    qint64 utf8Bytes() const { return m_utf8Bytes; }
    QString statusText() const { return m_statusText; }
    bool statusIsError() const { return m_statusIsError; }
    QString fontFamily() const { return m_fontFamily; }
    qreal fontSize() const { return m_fontSize; }
    qreal columnWidth() const;
    QStringList recentFiles() const { return m_recentFiles; }
    bool spellCheckAvailable() const { return m_spellChecker && m_spellChecker->isAvailable(); }
    bool spellCheckEnabled() const { return m_spellCheckEnabled && spellCheckAvailable(); }
    bool conflictPending() const { return m_conflictPending; }
    bool canRevert() const { return !m_filePath.isEmpty(); }
    QString searchQuery() const { return m_searchQuery; }
    int searchMatchCount() const { return static_cast<int>(m_searchMatches.size()); }
    int currentSearchIndex() const { return m_currentSearchIndex; }

    QString documentText() const;
    QTextDocument *textDocument() const;
    MarkdownHighlighter *highlighter() const;

    // QML hooks.
    Q_INVOKABLE void attachDocument(QQuickTextDocument *quickDoc);
    void attachQTextDocument(QTextDocument *doc);

    Q_INVOKABLE void newWindow();
    Q_INVOKABLE void openDialog();
    Q_INVOKABLE bool openFile(const QString &path);
    Q_INVOKABLE bool openOrStartFile(const QString &path);
    Q_INVOKABLE bool save();
    Q_INVOKABLE void saveAsDialog();
    Q_INVOKABLE bool saveToPath(const QString &path, bool isSaveAs = false);
    Q_INVOKABLE bool revertToSaved();
    Q_INVOKABLE void printDocument();

    Q_INVOKABLE void requestClose();
    Q_INVOKABLE void confirmDiscardAndClose();
    Q_INVOKABLE void confirmSaveAndClose();
    Q_INVOKABLE void cancelPendingClose();
    Q_INVOKABLE void keepLocalVersion();

    // Smart Markdown editing commands (each is a single Undo step).
    Q_INVOKABLE QVariantMap handleReturn(int selStart, int selEnd, bool shiftPressed);
    Q_INVOKABLE QVariantMap handleBackspace(int selStart, int selEnd);
    Q_INVOKABLE QVariantMap applyBold(int selStart, int selEnd);
    Q_INVOKABLE QVariantMap applyItalic(int selStart, int selEnd);
    Q_INVOKABLE QVariantMap applyLink(int selStart, int selEnd);
    Q_INVOKABLE QVariantMap handlePaste(int selStart, int selEnd);

    // Typography.
    Q_INVOKABLE void increaseTextSize();
    Q_INVOKABLE void decreaseTextSize();
    Q_INVOKABLE void resetTextSize();

    // Find & Replace.
    Q_INVOKABLE QVariantMap setSearchQuery(const QString &query, int fromPos = 0);
    Q_INVOKABLE QVariantMap findNext(int fromPos = -1);
    Q_INVOKABLE QVariantMap findPrevious(int fromPos = -1);
    Q_INVOKABLE QString selectionForFind(int selStart, int selEnd) const;
    Q_INVOKABLE QVariantMap replaceCurrent(const QString &replacement);
    Q_INVOKABLE int replaceAll(const QString &replacement);
    Q_INVOKABLE void clearSearch();

    // Spell checking.
    Q_INVOKABLE void toggleSpellCheck();
    Q_INVOKABLE QVariantMap spellingContextAt(int position) const;
    Q_INVOKABLE void replaceWordRange(int start, int length, const QString &replacement);
    Q_INVOKABLE void ignoreWord(const QString &word);

    // Recent files.
    Q_INVOKABLE void clearRecentFiles();

    // Recovery on startup when launched with no file argument.
    Q_INVOKABLE bool tryRecoverDraft();

    // For tests & language updates.
    void setLanguage(const QString &languageCode);
    void flushAutoSaveNow();
    void flushRecoveryNow();
    bool hasPendingRecovery() const { return m_recoveryTimer.isActive(); }

public slots:
    void retranslate();

signals:
    void documentChanged();
    void modifiedChanged();
    void wordCountChanged();
    void statusChanged();
    void typographyChanged();
    void recentFilesChanged();
    void spellCheckChanged();
    void conflictChanged();
    void searchChanged();
    void unsavedChangesRequested();
    void externalChangeDetected(bool deleted, bool locallyModified);
    void closeApproved();
    void selectionRequested(int start, int end);

private slots:
    void handleContentsChange(int position, int charsRemoved, int charsAdded);
    void handleContentsChanged();
    void handleThemeChanged();
    void handleAutoSaveTimer();
    void handleRecoveryTimer();
    void handleWatchedFileChanged();
    void handlePickerOpenSelected(const QUrl &url);
    void handlePickerSaveSelected(const QUrl &url);
    void handlePickerCancelled(bool forSave);
    void handlePickerFailed(const QString &message, bool forSave);

private:
    void loadTextIntoDocument(const QString &text);
    void applyDocumentBaseTypography();
    QVariantMap applyMutation(const TextMutation &mutation);
    bool checkSizeDelta(int removeStart, int removeLen, const QString &replacement);
    void setStatus(const QString &text, bool isError = false);
    void setModified(bool modified);
    void watchCurrentFile();
    void unwatchCurrentFile();
    void refreshSearchMatches(int anchorPos, bool keepIndex = false);
    QVariantMap currentMatchMap() const;
    QString settingsPath() const;
    void loadSettings();
    void saveSettings() const;
    QFont baseFont() const;

    Theme *m_theme = nullptr;
    FilePicker *m_filePicker = nullptr;
    Printer *m_printer = nullptr;
    DocumentStore m_store;
    QString m_configDir;
    std::unique_ptr<SpellChecker> m_spellChecker;

    QPointer<QTextDocument> m_document;
    QPointer<MarkdownHighlighter> m_highlighter;

    QString m_filePath;
    QString m_cachedText;
    TextMetrics m_metrics;
    qint64 m_utf8Bytes = 0;
    QByteArray m_baselineSha256;
    QByteArray m_recordedBaseline;
    bool m_hasConflictBaseline = false;

    bool m_modified = false;
    bool m_loading = false;
    bool m_handledContentsChange = false;
    bool m_undoingOversized = false;
    bool m_conflictPending = false;
    bool m_autoSaveSuspended = false;
    bool m_closePending = false;
    bool m_closeAfterSave = false;

    QString m_statusText;
    bool m_statusIsError = false;

    QString m_fontFamily;
    qreal m_fontSize = kDefaultFontSize;
    bool m_spellCheckEnabled = true;

    QStringList m_recentFiles;

    QString m_searchQuery;
    QList<int> m_searchMatches;
    int m_currentSearchIndex = -1;

    QTimer m_autoSaveTimer;
    QTimer m_recoveryTimer;
    QFileSystemWatcher m_fileWatcher;
    QTimer m_fileCheckTimer;
};
