#include "backend.h"

#include "filepicker.h"
#include "highlighter.h"
#include "printer.h"
#include "theme.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QProcess>
#include <QQuickTextDocument>
#include <QSaveFile>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kAutoSaveDelayMs = 10000;
constexpr int kRecoveryDelayMs = 2000;
} // namespace

Backend::Backend(Theme *theme, FilePicker *filePicker, Printer *printer,
                 const QString &stateDirectory, const QString &configDirectory,
                 const QStringList &spellSearchDirs, QObject *parent)
    : QObject(parent),
      m_theme(theme),
      m_filePicker(filePicker),
      m_printer(printer),
      m_store(stateDirectory),
      m_configDir(configDirectory.isEmpty()
                      ? (qEnvironmentVariableIsEmpty("XDG_CONFIG_HOME")
                             ? QDir::homePath() + QStringLiteral("/.config/gonzo")
                             : qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/gonzo"))
                      : configDirectory),
      m_spellChecker(std::make_unique<SpellChecker>(QString(), spellSearchDirs)),
      m_statusText(tr("Markdown")) {
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_fontFamily = fixed.family().isEmpty() ? QStringLiteral("monospace") : fixed.family();

    loadSettings();
    m_recentFiles = m_store.recentFiles();

    m_autoSaveTimer.setSingleShot(true);
    m_autoSaveTimer.setInterval(kAutoSaveDelayMs);
    connect(&m_autoSaveTimer, &QTimer::timeout, this, &Backend::handleAutoSaveTimer);

    m_recoveryTimer.setSingleShot(true);
    m_recoveryTimer.setInterval(kRecoveryDelayMs);
    connect(&m_recoveryTimer, &QTimer::timeout, this, &Backend::handleRecoveryTimer);

    m_fileCheckTimer.setSingleShot(true);
    m_fileCheckTimer.setInterval(120);
    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged, this, [this] { m_fileCheckTimer.start(); });
    connect(&m_fileWatcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_fileCheckTimer.start(); });
    connect(&m_fileCheckTimer, &QTimer::timeout, this, &Backend::handleWatchedFileChanged);

    if (m_theme)
        connect(m_theme, &Theme::changed, this, &Backend::handleThemeChanged);

    if (m_filePicker) {
        connect(m_filePicker, &FilePicker::openSelected, this, &Backend::handlePickerOpenSelected);
        connect(m_filePicker, &FilePicker::saveSelected, this, &Backend::handlePickerSaveSelected);
        connect(m_filePicker, &FilePicker::cancelled, this, &Backend::handlePickerCancelled);
        connect(m_filePicker, &FilePicker::failed, this, &Backend::handlePickerFailed);
    }

    if (m_printer) {
        connect(m_printer, &Printer::printSucceeded, this, [this] { setStatus(tr("Printed")); });
        connect(m_printer, &Printer::printCancelled, this, [this] { setStatus(tr("Markdown")); });
        connect(m_printer, &Printer::printFailed, this, [this](const QString &msg) { setStatus(msg, true); });
    }
}

Backend::~Backend() {
    if (!m_modified)
        m_store.clearRecoveryDraft();
}

QString Backend::fileName() const {
    if (m_filePath.isEmpty())
        return tr("Untitled");
    return QFileInfo(m_filePath).fileName();
}

QString Backend::displayTitle() const {
    const QString prefix = m_modified ? QStringLiteral("• ") : QString();
    return prefix + fileName() + QStringLiteral(" — Gonzo");
}

qreal Backend::columnWidth() const {
    // Match gonzo-mac's ~65-character centered writing column.
    const QFontMetricsF fm(baseFont());
    const qreal charWidth = fm.horizontalAdvance(QLatin1Char('M'));
    const qreal measured = charWidth > 0.0 ? charWidth * 65.0 : m_fontSize * 0.6 * 65.0;
    return std::round(measured);
}

QString Backend::documentText() const {
    return m_document ? m_cachedText : QString();
}

QTextDocument *Backend::textDocument() const {
    return m_document;
}

MarkdownHighlighter *Backend::highlighter() const {
    return m_highlighter;
}

QFont Backend::baseFont() const {
    QFont f(m_fontFamily);
    f.setStyleHint(QFont::Monospace);
    f.setPointSizeF(m_fontSize);
    return f;
}

void Backend::attachDocument(QQuickTextDocument *quickDoc) {
    if (!quickDoc)
        return;
    attachQTextDocument(quickDoc->textDocument());
}

void Backend::attachQTextDocument(QTextDocument *doc) {
    if (!doc || m_document == doc)
        return;

    if (m_document)
        disconnect(m_document, nullptr, this, nullptr);

    m_document = doc;
    m_document->setDocumentMargin(0);
    m_document->setDefaultFont(baseFont());

    m_highlighter = new MarkdownHighlighter(m_document);
    m_highlighter->setBaseFont(baseFont());
    if (m_theme)
        m_highlighter->setTheme(m_theme);
    m_highlighter->setSpellChecker(m_spellChecker.get(), spellCheckEnabled());

    connect(m_document, &QTextDocument::contentsChange, this, &Backend::handleContentsChange);
    connect(m_document, &QTextDocument::contentsChanged, this, &Backend::handleContentsChanged);

    if (!m_cachedText.isEmpty()) {
        loadTextIntoDocument(m_cachedText);
    } else {
        applyDocumentBaseTypography();
    }
}

void Backend::loadTextIntoDocument(const QString &text) {
    m_cachedText = text;
    m_metrics.reset(m_cachedText);
    m_utf8Bytes = m_cachedText.toUtf8().size();

    if (m_document) {
        m_loading = true;
        m_document->setUndoRedoEnabled(false);
        m_document->setPlainText(m_cachedText);
        applyDocumentBaseTypography();
        m_document->setUndoRedoEnabled(true);
        m_document->clearUndoRedoStacks();
        m_document->setModified(false);
        m_loading = false;
        if (m_highlighter)
            m_highlighter->rehighlight();
    }

    if (!m_searchQuery.isEmpty())
        refreshSearchMatches(0);
    emit wordCountChanged();
}

void Backend::applyDocumentBaseTypography() {
    if (!m_document)
        return;
    const QFont f = baseFont();
    m_document->setDefaultFont(f);

    const bool wasLoading = m_loading;
    m_loading = true;
    const bool undoEnabled = m_document->isUndoRedoEnabled();
    m_document->setUndoRedoEnabled(false);

    QTextBlockFormat blockFmt;
    blockFmt.setLineHeight(155.0, QTextBlockFormat::ProportionalHeight);
    blockFmt.setBottomMargin(2.0);
    QTextCursor cursor(m_document);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(blockFmt);

    m_document->setUndoRedoEnabled(undoEnabled);
    m_loading = wasLoading;

    if (m_highlighter)
        m_highlighter->setBaseFont(f);
}

void Backend::handleContentsChange(int position, int charsRemoved, int charsAdded) {
    if (m_loading || !m_document)
        return;
    if (charsRemoved == 0 && charsAdded == 0)
        return;
    m_handledContentsChange = true;

    QString inserted;
    if (charsAdded > 0) {
        QTextCursor cursor(m_document);
        const int docLen = m_document->characterCount() - 1;
        const int safePos = std::clamp(position, 0, std::max(0, docLen));
        const int safeEnd = std::clamp(safePos + charsAdded, safePos, std::max(0, docLen));
        cursor.setPosition(safePos);
        cursor.setPosition(safeEnd, QTextCursor::KeepAnchor);
        inserted = cursor.selectedText();
        inserted.replace(QChar(0x2029), QLatin1Char('\n'));
    }

    const int docLen = std::max(0, m_document->characterCount() - 1);
    const bool incrementalValid = position >= 0
        && charsRemoved >= 0
        && position + charsRemoved <= m_cachedText.size()
        && m_cachedText.size() - charsRemoved + inserted.size() == docLen;

    if (incrementalValid) {
        const qint64 removedBytes = m_cachedText.mid(position, charsRemoved).toUtf8().size();
        const qint64 addedBytes = inserted.toUtf8().size();
        m_cachedText.replace(position, charsRemoved, inserted);
        m_utf8Bytes += addedBytes - removedBytes;
        m_metrics.applyEdit(m_cachedText, position, charsRemoved, static_cast<int>(inserted.size()));
    } else {
        m_cachedText = m_document->toPlainText();
        m_utf8Bytes = m_cachedText.toUtf8().size();
        m_metrics.reset(m_cachedText);
    }

    if (m_utf8Bytes > DocumentStore::kMaxFileBytes && !m_undoingOversized) {
        m_undoingOversized = true;
        setStatus(tr("This document exceeds Gonzo's 16 MB limit."), true);
        QMetaObject::invokeMethod(this, [this] {
            if (m_document && m_document->isUndoAvailable())
                m_document->undo();
            m_undoingOversized = false;
        }, Qt::QueuedConnection);
        return;
    }

    emit wordCountChanged();
    setModified(true);
    if (!m_conflictPending)
        setStatus(tr("Markdown"));

    m_recoveryTimer.start();
    if (!m_filePath.isEmpty() && !m_autoSaveSuspended && !m_conflictPending)
        m_autoSaveTimer.start();

    if (!m_searchQuery.isEmpty())
        refreshSearchMatches(position, true);
}

void Backend::handleContentsChanged() {
    if (m_loading || !m_document)
        return;
    const int docLen = std::max(0, m_document->characterCount() - 1);
    if (m_handledContentsChange && m_cachedText.size() == docLen) {
        m_handledContentsChange = false;
        return;
    }
    m_handledContentsChange = false;

    // Fallback for bulk QTextDocument operations like setPlainText()/clear()
    // that emit contentsChanged() without contentsChange(pos, removed, added).
    m_cachedText = m_document->toPlainText();
    m_utf8Bytes = m_cachedText.toUtf8().size();
    m_metrics.reset(m_cachedText);
    emit wordCountChanged();
    setModified(true);
    if (!m_conflictPending)
        setStatus(tr("Markdown"));
    m_recoveryTimer.start();
    if (!m_filePath.isEmpty() && !m_autoSaveSuspended && !m_conflictPending)
        m_autoSaveTimer.start();
    if (!m_searchQuery.isEmpty())
        refreshSearchMatches(0, false);
}

void Backend::handleThemeChanged() {
    if (m_highlighter && m_theme)
        m_highlighter->setTheme(m_theme);
}

void Backend::setStatus(const QString &text, bool isError) {
    if (m_statusText == text && m_statusIsError == isError)
        return;
    m_statusText = text;
    m_statusIsError = isError;
    emit statusChanged();
}

void Backend::setModified(bool modified) {
    if (m_modified == modified)
        return;
    m_modified = modified;
    emit modifiedChanged();
    emit documentChanged();
}

void Backend::newWindow() {
    const QString exe = QCoreApplication::applicationFilePath();
    if (!QProcess::startDetached(exe, {}))
        setStatus(tr("Could not open a new window."), true);
}

void Backend::openDialog() {
    if (!m_filePicker) {
        setStatus(tr("File chooser is unavailable."), true);
        return;
    }
    const QString folder = m_filePath.isEmpty() ? QDir::homePath() : QFileInfo(m_filePath).absolutePath();
    m_filePicker->openDocument(folder);
}

bool Backend::openFile(const QString &path) {
    const ReadFileResult res = DocumentStore::readFile(path);
    if (!res.ok) {
        setStatus(res.error, true);
        return false;
    }

    unwatchCurrentFile();
    m_autoSaveTimer.stop();
    m_recoveryTimer.stop();
    m_store.clearRecoveryDraft();

    m_filePath = res.resolvedPath;
    m_baselineSha256 = res.sha256;
    m_recordedBaseline = res.sha256;
    m_hasConflictBaseline = true;
    m_conflictPending = false;
    m_autoSaveSuspended = false;

    loadTextIntoDocument(res.text);
    setModified(false);
    setStatus(tr("Markdown"));

    m_store.addRecentFile(m_filePath);
    m_recentFiles = m_store.recentFiles();
    watchCurrentFile();

    emit conflictChanged();
    emit recentFilesChanged();
    emit documentChanged();
    emit selectionRequested(0, 0);
    return true;
}

bool Backend::openOrStartFile(const QString &path) {
    if (path.isEmpty())
        return false;
    const QFileInfo info(path);
    if (info.exists())
        return openFile(path);

    // Starting a new named file from CLI: target path is recorded without writing until save.
    unwatchCurrentFile();
    m_filePath = DocumentStore::resolveTargetPath(path);
    m_baselineSha256.clear();
    m_recordedBaseline.clear();
    m_hasConflictBaseline = false;
    m_conflictPending = false;
    m_autoSaveSuspended = false;
    loadTextIntoDocument(QString());
    setModified(false);
    setStatus(tr("Markdown"));
    emit conflictChanged();
    emit documentChanged();
    return true;
}

bool Backend::save() {
    if (m_filePath.isEmpty()) {
        saveAsDialog();
        return false;
    }
    return saveToPath(m_filePath, false);
}

void Backend::saveAsDialog() {
    if (!m_filePicker) {
        setStatus(tr("File chooser is unavailable."), true);
        return;
    }
    const QUrl suggested = m_filePath.isEmpty()
        ? QUrl::fromLocalFile(QDir::homePath() + QStringLiteral("/Untitled.md"))
        : QUrl::fromLocalFile(m_filePath);
    m_filePicker->saveDocument(suggested);
}

bool Backend::saveToPath(const QString &path, bool isSaveAs) {
    const QByteArray utf8 = m_cachedText.toUtf8();
    const bool checkConflict = !isSaveAs && m_hasConflictBaseline;

    unwatchCurrentFile();
    const WriteFileResult res = DocumentStore::writeFile(
        path, utf8, checkConflict ? m_baselineSha256 : QByteArray(), checkConflict);

    if (!res.ok) {
        if (!m_filePath.isEmpty())
            watchCurrentFile();
        if (res.conflict) {
            m_conflictPending = true;
            m_autoSaveSuspended = true;
            m_autoSaveTimer.stop();
            emit conflictChanged();
            setStatus(res.error, true);
            emit externalChangeDetected(false, m_modified);
        } else {
            setStatus(res.error, true);
        }
        m_closeAfterSave = false;
        return false;
    }

    m_filePath = res.resolvedPath;
    m_baselineSha256 = res.sha256;
    m_recordedBaseline = res.sha256;
    m_hasConflictBaseline = true;
    m_conflictPending = false;
    m_autoSaveSuspended = false;
    m_autoSaveTimer.stop();
    m_recoveryTimer.stop();
    m_store.clearRecoveryDraft();

    setModified(false);
    setStatus(tr("Saved"));
    m_store.addRecentFile(m_filePath);
    m_recentFiles = m_store.recentFiles();
    watchCurrentFile();

    emit conflictChanged();
    emit recentFilesChanged();
    emit documentChanged();

    if (m_closeAfterSave) {
        m_closeAfterSave = false;
        m_closePending = false;
        emit closeApproved();
    }
    return true;
}

bool Backend::revertToSaved() {
    if (m_filePath.isEmpty())
        return false;
    return openFile(m_filePath);
}

void Backend::keepLocalVersion() {
    m_conflictPending = false;
    m_autoSaveSuspended = true;
    emit conflictChanged();
    setStatus(tr("Use Save As to keep your edits in another file."));
}

void Backend::printDocument() {
    if (!m_printer) {
        setStatus(tr("Printing is unavailable."), true);
        return;
    }
    m_printer->printMarkdownSource(fileName(), m_cachedText);
}

void Backend::requestClose() {
    if (!m_modified) {
        m_store.clearRecoveryDraft();
        emit closeApproved();
        return;
    }
    m_closePending = true;
    emit unsavedChangesRequested();
}

void Backend::confirmDiscardAndClose() {
    m_closePending = false;
    m_closeAfterSave = false;
    m_autoSaveTimer.stop();
    m_recoveryTimer.stop();
    m_store.clearRecoveryDraft();
    setModified(false);
    emit closeApproved();
}

void Backend::confirmSaveAndClose() {
    m_closeAfterSave = true;
    if (m_filePath.isEmpty()) {
        saveAsDialog();
    } else {
        save();
    }
}

void Backend::cancelPendingClose() {
    m_closePending = false;
    m_closeAfterSave = false;
}

bool Backend::checkSizeDelta(int removeStart, int removeLen, const QString &replacement) {
    const int safeStart = std::clamp(removeStart, 0, static_cast<int>(m_cachedText.size()));
    const int safeLen = std::clamp(removeLen, 0, static_cast<int>(m_cachedText.size()) - safeStart);
    const qint64 removedBytes = m_cachedText.mid(safeStart, safeLen).toUtf8().size();
    const qint64 addedBytes = replacement.toUtf8().size();
    if (m_utf8Bytes - removedBytes + addedBytes > DocumentStore::kMaxFileBytes) {
        setStatus(tr("This document exceeds Gonzo's 16 MB limit."), true);
        return false;
    }
    return true;
}

QVariantMap Backend::applyMutation(const TextMutation &mutation) {
    QVariantMap out;
    out.insert(QStringLiteral("handled"), false);
    if (!m_document)
        return out;
    if (!checkSizeDelta(mutation.range.start, mutation.range.length, mutation.replacement))
        return out;

    const int docLen = std::max(0, m_document->characterCount() - 1);
    const int start = std::clamp(mutation.range.start, 0, docLen);
    const int end = std::clamp(mutation.range.end(), start, docLen);

    QTextCursor cursor(m_document);
    cursor.beginEditBlock();
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.insertText(mutation.replacement);
    cursor.endEditBlock();

    const int selStart = std::clamp(mutation.selection.start, 0, static_cast<int>(m_cachedText.size()));
    const int selEnd = std::clamp(mutation.selection.end(), selStart, static_cast<int>(m_cachedText.size()));
    out.insert(QStringLiteral("handled"), true);
    out.insert(QStringLiteral("selectionStart"), selStart);
    out.insert(QStringLiteral("selectionEnd"), selEnd);
    emit selectionRequested(selStart, selEnd);
    return out;
}

QVariantMap Backend::handleReturn(int selStart, int selEnd, bool shiftPressed) {
    const int s = std::clamp(std::min(selStart, selEnd), 0, static_cast<int>(m_cachedText.size()));
    const int e = std::clamp(std::max(selStart, selEnd), s, static_cast<int>(m_cachedText.size()));
    const TextRange para = m_metrics.paragraphRange(m_cachedText, TextRange{s, 0});
    const TextMutation mutation = EditorCommands::newline(
        m_cachedText, TextRange{s, e - s}, shiftPressed, para);
    return applyMutation(mutation);
}

QVariantMap Backend::handleBackspace(int selStart, int selEnd) {
    const int s = std::clamp(std::min(selStart, selEnd), 0, static_cast<int>(m_cachedText.size()));
    const int e = std::clamp(std::max(selStart, selEnd), s, static_cast<int>(m_cachedText.size()));
    const auto mutation = EditorCommands::deleteBackward(m_cachedText, TextRange{s, e - s});
    if (!mutation.has_value())
        return {{QStringLiteral("handled"), false}};
    return applyMutation(*mutation);
}

QVariantMap Backend::applyBold(int selStart, int selEnd) {
    const int s = std::clamp(std::min(selStart, selEnd), 0, static_cast<int>(m_cachedText.size()));
    const int e = std::clamp(std::max(selStart, selEnd), s, static_cast<int>(m_cachedText.size()));
    return applyMutation(EditorCommands::wrap(m_cachedText, TextRange{s, e - s}, QStringLiteral("**")));
}

QVariantMap Backend::applyItalic(int selStart, int selEnd) {
    const int s = std::clamp(std::min(selStart, selEnd), 0, static_cast<int>(m_cachedText.size()));
    const int e = std::clamp(std::max(selStart, selEnd), s, static_cast<int>(m_cachedText.size()));
    return applyMutation(EditorCommands::wrap(m_cachedText, TextRange{s, e - s}, QStringLiteral("*")));
}

QVariantMap Backend::applyLink(int selStart, int selEnd) {
    const int s = std::clamp(std::min(selStart, selEnd), 0, static_cast<int>(m_cachedText.size()));
    const int e = std::clamp(std::max(selStart, selEnd), s, static_cast<int>(m_cachedText.size()));
    std::optional<QString> dest;
    if (const QClipboard *cb = QGuiApplication::clipboard())
        dest = EditorCommands::linkDestination(cb->text());
    return applyMutation(EditorCommands::link(m_cachedText, TextRange{s, e - s}, dest));
}

QVariantMap Backend::handlePaste(int selStart, int selEnd) {
    const QClipboard *cb = QGuiApplication::clipboard();
    if (!cb)
        return {{QStringLiteral("handled"), false}};
    const QString text = cb->text();
    if (text.isEmpty())
        return {{QStringLiteral("handled"), true},
                {QStringLiteral("selectionStart"), selStart},
                {QStringLiteral("selectionEnd"), selEnd}};

    // Each UTF-16 unit is at least one UTF-8 byte; reject oversized pastes upfront.
    if (text.size() > DocumentStore::kMaxFileBytes) {
        setStatus(tr("This document exceeds Gonzo's 16 MB limit."), true);
        return {{QStringLiteral("handled"), true},
                {QStringLiteral("selectionStart"), selStart},
                {QStringLiteral("selectionEnd"), selEnd}};
    }

    const int s = std::clamp(std::min(selStart, selEnd), 0, static_cast<int>(m_cachedText.size()));
    const int e = std::clamp(std::max(selStart, selEnd), s, static_cast<int>(m_cachedText.size()));
    if (e > s) {
        if (const auto dest = EditorCommands::linkDestination(text); dest.has_value())
            return applyMutation(EditorCommands::link(m_cachedText, TextRange{s, e - s}, dest));
    }

    const QString normalized = EditorCommands::normalize(text);
    const TextMutation mutation{
        TextRange{s, e - s},
        normalized,
        TextRange{s + static_cast<int>(normalized.size()), 0}
    };
    return applyMutation(mutation);
}

void Backend::increaseTextSize() {
    const qreal next = std::min(kMaxFontSize, m_fontSize + 1.0);
    if (next == m_fontSize)
        return;
    m_fontSize = next;
    applyDocumentBaseTypography();
    saveSettings();
    emit typographyChanged();
}

void Backend::decreaseTextSize() {
    const qreal next = std::max(kMinFontSize, m_fontSize - 1.0);
    if (next == m_fontSize)
        return;
    m_fontSize = next;
    applyDocumentBaseTypography();
    saveSettings();
    emit typographyChanged();
}

void Backend::resetTextSize() {
    if (m_fontSize == kDefaultFontSize)
        return;
    m_fontSize = kDefaultFontSize;
    applyDocumentBaseTypography();
    saveSettings();
    emit typographyChanged();
}

void Backend::refreshSearchMatches(int anchorPos, bool keepIndex) {
    m_searchMatches.clear();
    if (!m_searchQuery.isEmpty()) {
        const int qLen = static_cast<int>(m_searchQuery.size());
        qsizetype idx = 0;
        while ((idx = m_cachedText.indexOf(m_searchQuery, idx, Qt::CaseInsensitive)) >= 0) {
            m_searchMatches.append(static_cast<int>(idx));
            idx += std::max(1, qLen);
        }
    }

    if (m_searchMatches.isEmpty()) {
        m_currentSearchIndex = -1;
    } else if (keepIndex && m_currentSearchIndex >= 0 && m_currentSearchIndex < m_searchMatches.size()) {
        // Keep current index if still valid.
    } else {
        m_currentSearchIndex = 0;
        for (int i = 0; i < m_searchMatches.size(); ++i) {
            if (m_searchMatches.at(i) >= anchorPos) {
                m_currentSearchIndex = i;
                break;
            }
        }
    }

    if (m_highlighter) {
        const int curStart = (m_currentSearchIndex >= 0 && m_currentSearchIndex < m_searchMatches.size())
            ? m_searchMatches.at(m_currentSearchIndex)
            : -1;
        m_highlighter->setSearch(m_searchQuery, curStart);
    }
    emit searchChanged();
}

QVariantMap Backend::currentMatchMap() const {
    if (m_currentSearchIndex < 0 || m_currentSearchIndex >= m_searchMatches.size())
        return {{QStringLiteral("found"), false}};
    return {
        {QStringLiteral("found"), true},
        {QStringLiteral("start"), m_searchMatches.at(m_currentSearchIndex)},
        {QStringLiteral("length"), static_cast<int>(m_searchQuery.size())},
        {QStringLiteral("index"), m_currentSearchIndex},
        {QStringLiteral("count"), static_cast<int>(m_searchMatches.size())}
    };
}

QVariantMap Backend::setSearchQuery(const QString &query, int fromPos) {
    m_searchQuery = query;
    refreshSearchMatches(fromPos, false);
    return currentMatchMap();
}

QVariantMap Backend::findNext(int fromPos) {
    if (m_searchMatches.isEmpty())
        return currentMatchMap();
    if (fromPos >= 0 && m_currentSearchIndex < 0) {
        refreshSearchMatches(fromPos, false);
    } else {
        m_currentSearchIndex = (m_currentSearchIndex + 1) % m_searchMatches.size();
        if (m_highlighter)
            m_highlighter->setSearch(m_searchQuery, m_searchMatches.at(m_currentSearchIndex));
        emit searchChanged();
    }
    return currentMatchMap();
}

QVariantMap Backend::findPrevious(int fromPos) {
    if (m_searchMatches.isEmpty())
        return currentMatchMap();
    if (fromPos >= 0 && m_currentSearchIndex < 0) {
        refreshSearchMatches(fromPos, false);
    } else {
        m_currentSearchIndex = (m_currentSearchIndex - 1 + m_searchMatches.size()) % m_searchMatches.size();
        if (m_highlighter)
            m_highlighter->setSearch(m_searchQuery, m_searchMatches.at(m_currentSearchIndex));
        emit searchChanged();
    }
    return currentMatchMap();
}

QString Backend::selectionForFind(int selStart, int selEnd) const {
    const int s = std::clamp(std::min(selStart, selEnd), 0, static_cast<int>(m_cachedText.size()));
    const int e = std::clamp(std::max(selStart, selEnd), s, static_cast<int>(m_cachedText.size()));
    if (e <= s)
        return {};
    return m_cachedText.mid(s, std::min(e - s, 512));
}

QVariantMap Backend::replaceCurrent(const QString &replacement) {
    if (!m_document || m_currentSearchIndex < 0 || m_currentSearchIndex >= m_searchMatches.size())
        return currentMatchMap();
    const int start = m_searchMatches.at(m_currentSearchIndex);
    const int len = static_cast<int>(m_searchQuery.size());
    if (!checkSizeDelta(start, len, replacement))
        return currentMatchMap();

    QTextCursor cursor(m_document);
    cursor.beginEditBlock();
    cursor.setPosition(start);
    cursor.setPosition(start + len, QTextCursor::KeepAnchor);
    cursor.insertText(replacement);
    cursor.endEditBlock();

    refreshSearchMatches(start + static_cast<int>(replacement.size()), false);
    return currentMatchMap();
}

int Backend::replaceAll(const QString &replacement) {
    if (!m_document || m_searchMatches.isEmpty())
        return 0;
    const int qLen = static_cast<int>(m_searchQuery.size());
    const int count = static_cast<int>(m_searchMatches.size());
    const qint64 qBytes = m_searchQuery.toUtf8().size();
    const qint64 rBytes = replacement.toUtf8().size();
    if (m_utf8Bytes + static_cast<qint64>(count) * (rBytes - qBytes) > DocumentStore::kMaxFileBytes) {
        setStatus(tr("This document exceeds Gonzo's 16 MB limit."), true);
        return 0;
    }

    const QList<int> matches = m_searchMatches;
    QTextCursor cursor(m_document);
    cursor.beginEditBlock();
    for (int i = matches.size() - 1; i >= 0; --i) {
        cursor.setPosition(matches.at(i));
        cursor.setPosition(matches.at(i) + qLen, QTextCursor::KeepAnchor);
        cursor.insertText(replacement);
    }
    cursor.endEditBlock();

    refreshSearchMatches(0, false);
    return count;
}

void Backend::clearSearch() {
    m_searchQuery.clear();
    m_searchMatches.clear();
    m_currentSearchIndex = -1;
    if (m_highlighter)
        m_highlighter->setSearch(QString(), -1);
    emit searchChanged();
}

void Backend::toggleSpellCheck() {
    if (!spellCheckAvailable()) {
        setStatus(tr("No Hunspell dictionary installed."));
        return;
    }
    m_spellCheckEnabled = !m_spellCheckEnabled;
    if (m_highlighter)
        m_highlighter->setSpellChecker(m_spellChecker.get(), spellCheckEnabled());
    saveSettings();
    emit spellCheckChanged();
    setStatus(spellCheckEnabled() ? tr("Spell check on") : tr("Spell check off"));
}

QVariantMap Backend::spellingContextAt(int position) const {
    QVariantMap out{{QStringLiteral("misspelled"), false}};
    if (!spellCheckEnabled() || !m_document)
        return out;
    const QTextBlock block = m_document->findBlock(position);
    if (!block.isValid())
        return out;
    const int localPos = position - block.position();
    const QString text = block.text();
    for (const TextRange &r : m_spellChecker->misspelledRanges(text)) {
        if (localPos >= r.start && localPos <= r.end()) {
            const QString word = text.mid(r.start, r.length);
            out.insert(QStringLiteral("misspelled"), true);
            out.insert(QStringLiteral("word"), word);
            out.insert(QStringLiteral("start"), block.position() + r.start);
            out.insert(QStringLiteral("length"), r.length);
            out.insert(QStringLiteral("suggestions"), m_spellChecker->suggestions(word));
            return out;
        }
    }
    return out;
}

void Backend::replaceWordRange(int start, int length, const QString &replacement) {
    applyMutation(TextMutation{
        TextRange{start, length},
        replacement,
        TextRange{start + static_cast<int>(replacement.size()), 0}
    });
}

void Backend::ignoreWord(const QString &word) {
    if (!m_spellChecker || word.isEmpty())
        return;
    m_spellChecker->ignoreWord(word);
    if (m_highlighter)
        m_highlighter->rehighlight();
}

void Backend::clearRecentFiles() {
    m_store.clearRecentFiles();
    m_recentFiles.clear();
    emit recentFilesChanged();
}

bool Backend::tryRecoverDraft() {
    const auto draft = m_store.takeRecoverableDraft();
    if (!draft.has_value())
        return false;

    unwatchCurrentFile();
    m_filePath = draft->originalPath;
    m_recordedBaseline = draft->recordedBaseline;
    m_hasConflictBaseline = draft->hasConflictBaseline;
    m_baselineSha256 = draft->conflictBaseline;
    m_conflictPending = false;
    // Never auto-save a recovered draft over its original until explicitly saved.
    m_autoSaveSuspended = true;

    loadTextIntoDocument(draft->text);
    setModified(true);
    setStatus(tr("Recovered draft — save to keep changes."));
    if (!m_filePath.isEmpty())
        watchCurrentFile();

    emit conflictChanged();
    emit documentChanged();
    return true;
}

void Backend::setLanguage(const QString &languageCode) {
    if (m_spellChecker) {
        m_spellChecker->setLanguage(languageCode);
        if (m_highlighter)
            m_highlighter->setSpellChecker(m_spellChecker.get(), spellCheckEnabled());
        emit spellCheckChanged();
    }
}

void Backend::retranslate() {
    if (!m_statusIsError && !m_conflictPending)
        setStatus(tr("Markdown"));
    emit documentChanged();
}

void Backend::handleAutoSaveTimer() {
    if (m_filePath.isEmpty() || !m_modified || m_autoSaveSuspended || m_conflictPending)
        return;
    saveToPath(m_filePath, false);
}

void Backend::handleRecoveryTimer() {
    if (!m_modified)
        return;
    m_store.writeRecoveryDraft(m_filePath, m_cachedText,
                               !m_recordedBaseline.isEmpty() ? m_recordedBaseline : m_baselineSha256);
}

void Backend::flushAutoSaveNow() {
    m_autoSaveTimer.stop();
    handleAutoSaveTimer();
}

void Backend::flushRecoveryNow() {
    m_recoveryTimer.stop();
    handleRecoveryTimer();
}

void Backend::watchCurrentFile() {
    unwatchCurrentFile();
    if (m_filePath.isEmpty())
        return;
    const QFileInfo info(m_filePath);
    if (info.exists())
        m_fileWatcher.addPath(m_filePath);
    if (QFileInfo::exists(info.absolutePath()))
        m_fileWatcher.addPath(info.absolutePath());
}

void Backend::unwatchCurrentFile() {
    const QStringList paths = m_fileWatcher.files() + m_fileWatcher.directories();
    if (!paths.isEmpty())
        m_fileWatcher.removePaths(paths);
}

void Backend::handleWatchedFileChanged() {
    if (m_filePath.isEmpty() || !m_hasConflictBaseline)
        return;
    watchCurrentFile();
    bool deleted = false;
    if (DocumentStore::diskMatchesBaseline(m_filePath, m_baselineSha256, &deleted))
        return;
    if (m_conflictPending)
        return;

    m_conflictPending = true;
    m_autoSaveSuspended = true;
    m_autoSaveTimer.stop();
    emit conflictChanged();
    setStatus(deleted
                  ? tr("The file was moved or deleted outside Gonzo.")
                  : tr("The file changed outside Gonzo."),
              true);
    emit externalChangeDetected(deleted, m_modified);
}

void Backend::handlePickerOpenSelected(const QUrl &url) {
    if (url.isLocalFile())
        openFile(url.toLocalFile());
}

void Backend::handlePickerSaveSelected(const QUrl &url) {
    if (url.isLocalFile())
        saveToPath(url.toLocalFile(), true);
}

void Backend::handlePickerCancelled(bool forSave) {
    if (forSave && m_closeAfterSave)
        m_closeAfterSave = false;
}

void Backend::handlePickerFailed(const QString &message, bool forSave) {
    if (forSave && m_closeAfterSave)
        m_closeAfterSave = false;
    setStatus(message, true);
}

QString Backend::settingsPath() const {
    return m_configDir + QStringLiteral("/settings.json");
}

void Backend::loadSettings() {
    QFile file(settingsPath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
    if (obj.contains(QStringLiteral("fontSize")))
        m_fontSize = std::clamp(obj.value(QStringLiteral("fontSize")).toDouble(kDefaultFontSize),
                                kMinFontSize, kMaxFontSize);
    if (obj.contains(QStringLiteral("spellCheckEnabled")))
        m_spellCheckEnabled = obj.value(QStringLiteral("spellCheckEnabled")).toBool(true);
}

void Backend::saveSettings() const {
    QDir().mkpath(m_configDir);
    QJsonObject obj;
    obj.insert(QStringLiteral("fontSize"), m_fontSize);
    obj.insert(QStringLiteral("spellCheckEnabled"), m_spellCheckEnabled);
    QSaveFile file(settingsPath());
    const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size())
        file.commit();
}
