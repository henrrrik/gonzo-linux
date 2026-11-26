#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QtTest>

#include <random>
#include <sys/stat.h>

#include "backend.h"
#include "cli.h"
#include "documentstore.h"
#include "filepicker.h"
#include "highlighter.h"
#include "kineticscroller.h"
#include "markdown.h"
#include "portalprinter.h"
#include "printer.h"
#include "spellchecker.h"
#include "textmetrics.h"
#include "theme.h"
#include "translator.h"

namespace {

class FakeFilePicker : public FilePicker {
    Q_OBJECT
public:
    using FilePicker::FilePicker;
    QUrl nextOpenUrl;
    QUrl nextSaveUrl;
    int openCalls = 0;
    int saveCalls = 0;

    void openDocument(const QString &) override {
        ++openCalls;
        if (nextOpenUrl.isValid())
            emit openSelected(nextOpenUrl);
    }

    void saveDocument(const QUrl &) override {
        ++saveCalls;
        if (nextSaveUrl.isValid())
            emit saveSelected(nextSaveUrl);
    }
};

class FakePrinter : public Printer {
    Q_OBJECT
public:
    using Printer::Printer;
    QString lastTitle;
    QString lastText;
    int calls = 0;

    void printMarkdownSource(const QString &title, const QString &text) override {
        ++calls;
        lastTitle = title;
        lastText = text;
        emit printSucceeded();
    }
};

bool writeBytes(const QString &path, const QByteArray &data) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

} // namespace

class GonzoTests : public QObject {
    Q_OBJECT

private slots:
    // --- Markdown & EditorCommands (ported from gonzo-mac EditorTests) ---

    void wrappingKeepsUnicodeSelection() {
        const QString text = QStringLiteral("A 🦇 story");
        const int idx = static_cast<int>(text.indexOf(QStringLiteral("🦇 story")));
        const int len = static_cast<int>(QStringLiteral("🦇 story").size());
        const TextMutation res = EditorCommands::wrap(text, TextRange{idx, len}, QStringLiteral("**"));
        QCOMPARE(res.replacement, QStringLiteral("**🦇 story**"));
        QCOMPARE(res.selection, (TextRange{idx + 2, len}));
    }

    void linksEscapeSyntaxAndSelectDestination() {
        const QString text = QStringLiteral("[Fear]");
        const TextMutation res = EditorCommands::link(text, TextRange{0, static_cast<int>(text.size())}, std::nullopt);
        QCOMPARE(res.replacement, QStringLiteral("[\\[Fear\\]](https://)"));
        QCOMPARE(res.replacement.mid(res.selection.start, res.selection.length), QStringLiteral("https://"));

        const TextMutation pasted = EditorCommands::link(
            QStringLiteral("story"), TextRange{0, 5}, QStringLiteral("https://example.com/(notes)"));
        QCOMPARE(pasted.replacement, QStringLiteral("[story](https://example.com/\\(notes\\))"));
        QCOMPARE(pasted.selection.length, 0);
    }

    void paragraphAndListReturn() {
        struct Case { QString input; bool soft; QString expected; };
        const QList<Case> cases = {
            {QStringLiteral("The story"), false, QStringLiteral("\n\n")},
            {QStringLiteral("The story"), true, QStringLiteral("\n")},
            {QStringLiteral("- First"), false, QStringLiteral("\n- ")},
            {QStringLiteral("  9) First"), false, QStringLiteral("\n  10) ")},
            {QStringLiteral("> Quote"), false, QStringLiteral("\n> ")}
        };
        for (const auto &c : cases) {
            const TextMutation res = EditorCommands::newline(
                c.input, TextRange{static_cast<int>(c.input.size()), 0}, c.soft);
            QCOMPARE(res.replacement, c.expected);
        }
    }

    void emptyListReturnExitsListAndKeepsMarkerWithTrailingText() {
        const TextMutation exit = EditorCommands::newline(
            QStringLiteral("text\n\n- "), TextRange{8, 0}, false);
        QCOMPARE(exit.range, (TextRange{6, 2}));
        QCOMPARE(exit.replacement, QStringLiteral("\n"));

        const QString item = QStringLiteral("- item");
        const TextMutation afterMarker = EditorCommands::newline(item, TextRange{2, 0}, false);
        QCOMPARE(afterMarker.range, (TextRange{2, 0}));
        QCOMPARE(afterMarker.replacement, QStringLiteral("\n- "));

        const TextMutation blank = EditorCommands::newline(QStringLiteral("- \n"), TextRange{2, 0}, false);
        QCOMPARE(blank.range, (TextRange{0, 2}));
        QCOMPARE(blank.replacement, QStringLiteral("\n"));
    }

    void backspaceDeletesParagraphBreak() {
        const auto m = EditorCommands::deleteBackward(QStringLiteral("One\n\nTwo"), TextRange{5, 0});
        QVERIFY(m.has_value());
        QCOMPARE(m->range, (TextRange{3, 2}));
        QCOMPARE(m->replacement, QString());
        QVERIFY(!EditorCommands::deleteBackward(QStringLiteral("One\nTwo"), TextRange{4, 0}).has_value());
    }

    void markdownRangesUseUtf16AndRespectPrecedence() {
        const QString text = QStringLiteral("🦇 **fear** and `*code*`");
        const QList<MarkdownSpan> spans = Markdown::spans(text);
        bool sawBold = false;
        for (const auto &s : spans) {
            if (s.style == MarkdownStyle::Bold) {
                sawBold = true;
                QCOMPARE(text.mid(s.start, s.length), QStringLiteral("fear"));
                QCOMPARE(s.start, 5);
            }
        }
        QVERIFY(sawBold);
        QCOMPARE(spans.last().style, MarkdownStyle::Code);
    }

    void malformedMarkdownHasBoundedWorkAndAnchorsOnlyAtParagraphStart() {
        QElapsedTimer timer;
        timer.start();
        const QStringList adversarial = {
            QStringLiteral("[a](") + QString(40, QLatin1Char('\\')),
            QString(20000, QLatin1Char('[')),
            QString(4096, QLatin1Char('`')),
            QString(4096, QLatin1Char('_'))
        };
        for (const QString &src : adversarial)
            (void)Markdown::spans(src);
        QVERIFY(timer.elapsed() < 1000);

        const QString longLine = QString(20000, QLatin1Char('[')) + QStringLiteral("\n**short**");
        const QList<MarkdownSpan> spans = Markdown::spans(longLine);
        bool sawBold = false;
        for (const auto &s : spans)
            if (s.style == MarkdownStyle::Bold)
                sawBold = true;
        QVERIFY(sawBold);

        // U+000B, U+000C, U+0085 must never trigger block anchors mid-paragraph.
        for (const QString &src : {QStringLiteral("x\u000B> q"), QStringLiteral("x\u000C- item"),
                                   QStringLiteral("x\u0085# Title"), QStringLiteral("x\u000B***")}) {
            for (const auto &s : Markdown::spans(src)) {
                QVERIFY(s.style != MarkdownStyle::Quote);
                QVERIFY(s.style != MarkdownStyle::Marker);
                QVERIFY(s.style != MarkdownStyle::Heading);
            }
        }
    }

    void spellingExclusionsSkipCodeAndLinkDestinations() {
        const QString text = QStringLiteral("🦇 Teh `teh` [teh](https://teh.example/\\)x)\r\n**`code`**\n")
            + QString(5000, QLatin1Char('x')) + QStringLiteral(" `long`");
        const QList<TextRange> excl = Markdown::spellingExclusions(text);
        QStringList subs;
        for (const auto &r : excl)
            subs.append(text.mid(r.start, r.length));
        QCOMPARE(subs, (QStringList{QStringLiteral("`teh`"),
                                    QStringLiteral("https://teh.example/\\)x"),
                                    QStringLiteral("`code`")}));
    }

    // --- WordCounter & TextMetrics implicit treap ---

    void wordCounterHandlesUnicodeAndMarkdown() {
        QCOMPARE(WordCounter::count(QString()), 0);
        QCOMPARE(WordCounter::count(QStringLiteral("# **Hello** world\n\n- story")), 3);
        QCOMPARE(WordCounter::count(QStringLiteral("Fear & loathing — 1971")), 3);
        QCOMPARE(WordCounter::count(QStringLiteral("🦇 ✍️")), 0);
        QCOMPARE(WordCounter::count(QStringLiteral("Stockholm 東京 café")), 3);
        QCOMPARE(WordCounter::count(QStringLiteral("𐐀")), 1);
    }

    void textMetricsMatchesWordCounterAndParagraphsAcrossRandomEdits() {
        QString text = QStringLiteral("# Title\n\nFirst paragraph with 🦇 emoji and 東京 text.\r\nSecond line.\n");
        TextMetrics metrics(text);
        QCOMPARE(metrics.wordCount(), WordCounter::count(text));

        std::mt19937 rng(1337);
        const QStringList snippets = {
            QStringLiteral(" word "), QStringLiteral("𐐀"), QStringLiteral("\n\n"),
            QStringLiteral("\r\n"), QStringLiteral("café"), QStringLiteral(" — "),
            QString(1500, QLatin1Char('a'))
        };

        for (int step = 0; step < 80; ++step) {
            const int len = static_cast<int>(text.size());
            const int pos = len == 0 ? 0 : static_cast<int>(rng() % (len + 1));
            const int maxDel = std::min(len - pos, 64);
            const int delLen = maxDel == 0 ? 0 : static_cast<int>(rng() % (maxDel + 1));
            const QString ins = snippets.at(static_cast<qsizetype>(rng() % snippets.size()));

            text.replace(pos, delLen, ins);
            metrics.applyEdit(text, pos, delLen, static_cast<int>(ins.size()));
            QCOMPARE(metrics.wordCount(), WordCounter::count(text));

            const int probe = text.isEmpty() ? 0 : static_cast<int>(rng() % text.size());
            QCOMPARE(metrics.paragraphRange(text, TextRange{probe, 0}),
                     Paragraphs::range(text, TextRange{probe, 0}));
        }
    }

    // --- DocumentStore safety, SHA-256 conflict & recovery ---

    void documentStoreRoundTripsStripsBomAndRejectsInvalidUtf8() {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("story.md"));
        const QString expected = QStringLiteral("# Gonzo\n\nFear, café and 東京 🦇\n");
        QVERIFY(writeBytes(file, QByteArray("\xEF\xBB\xBF") + expected.toUtf8()));

        const ReadFileResult read = DocumentStore::readFile(file);
        QVERIFY(read.ok);
        QCOMPARE(read.text, expected);

        const QString badFile = dir.filePath(QStringLiteral("bad.md"));
        QVERIFY(writeBytes(badFile, QByteArray("\xFF\xFE\x00")));
        const ReadFileResult bad = DocumentStore::readFile(badFile);
        QVERIFY(!bad.ok);
    }

    void documentStoreFollowsSymlinksAndRejectsFifoWithoutBlocking() {
        QTemporaryDir dir;
        const QString target = dir.filePath(QStringLiteral("notes.md"));
        QVERIFY(writeBytes(target, QStringLiteral("# Linked 東京").toUtf8()));
        const QString link = dir.filePath(QStringLiteral("link.md"));
        QVERIFY(QFile::link(target, link));

        const ReadFileResult read = DocumentStore::readFile(link);
        QVERIFY(read.ok);
        QCOMPARE(read.text, QStringLiteral("# Linked 東京"));
        QCOMPARE(read.resolvedPath, QFileInfo(target).canonicalFilePath());

        const QString fifo = dir.filePath(QStringLiteral("pipe.md"));
        QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        const ReadFileResult fifoRead = DocumentStore::readFile(fifo);
        QVERIFY(!fifoRead.ok);
        const ReadFileResult dirRead = DocumentStore::readFile(dir.path());
        QVERIFY(!dirRead.ok);
    }

    void documentStorePreventsExternalOverwriteAndRecoversWithBaseline() {
        QTemporaryDir dir;
        const QString stateDir = dir.filePath(QStringLiteral("state"));
        const QString original = dir.filePath(QStringLiteral("original.md"));
        QVERIFY(writeBytes(original, "Original"));

        const ReadFileResult initial = DocumentStore::readFile(original);
        QVERIFY(initial.ok);

        {
            DocumentStore writer(stateDir);
            QVERIFY(writer.writeRecoveryDraft(initial.resolvedPath, QStringLiteral("Draft"), initial.sha256));
        }

        // Another app edits the original while Gonzo is closed.
        QVERIFY(writeBytes(original, "Changed while closed"));

        DocumentStore relaunched(stateDir);
        const auto recovered = relaunched.takeRecoverableDraft();
        QVERIFY(recovered.has_value());
        QCOMPARE(recovered->text, QStringLiteral("Draft"));
        QVERIFY(recovered->hasConflictBaseline);

        // Save against the original must refuse to overwrite external changes.
        const WriteFileResult blocked = DocumentStore::writeFile(
            recovered->originalPath, recovered->text.toUtf8(), recovered->conflictBaseline, true);
        QVERIFY(!blocked.ok);
        QVERIFY(blocked.conflict);
        QCOMPARE(DocumentStore::readFile(original).text, QStringLiteral("Changed while closed"));

        // Save As to a new path succeeds.
        const QString copy = dir.filePath(QStringLiteral("copy.md"));
        const WriteFileResult savedAs = DocumentStore::writeFile(copy, recovered->text.toUtf8(), {}, false);
        QVERIFY(savedAs.ok);
        QCOMPARE(DocumentStore::readFile(copy).text, QStringLiteral("Draft"));
    }

    // --- Highlighter & Backend ---

    void highlighterScalesHeadingsAndResetsWhenMarkerRemoved() {
        QTextDocument doc;
        Theme theme(QDir::tempPath() + QStringLiteral("/gonzo-nonexistent-theme"));
        Backend backend(&theme);
        backend.attachQTextDocument(&doc);
        doc.setPlainText(QStringLiteral("# Title\n\n## A *subtitle*\n\nBody text"));
        backend.highlighter()->rehighlight();

        auto pointSizeAt = [&](int blockNumber, int posInBlock) -> qreal {
            const QTextBlock block = doc.findBlockByNumber(blockNumber);
            const QList<QTextLayout::FormatRange> formats = block.layout()->formats();
            for (const auto &r : formats) {
                if (posInBlock >= r.start && posInBlock < r.start + r.length && r.format.fontPointSize() > 0)
                    return r.format.fontPointSize();
            }
            return doc.defaultFont().pointSizeF();
        };

        const qreal h1 = pointSizeAt(0, 3);
        const qreal h2 = pointSizeAt(2, 6);
        const qreal body = pointSizeAt(4, 2);
        QVERIFY(h1 > h2);
        QVERIFY(h2 > body);
        QCOMPARE(backend.wordCount(), 5);

        // Removing "# " resets the first block back to body font size.
        QTextCursor cursor(&doc);
        cursor.setPosition(0);
        cursor.setPosition(2, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        backend.highlighter()->rehighlight();
        QCOMPARE(pointSizeAt(0, 1), body);
    }

    void backendCommandsSupportSingleStepUndoFindReplaceAndFakePortals() {
        QTemporaryDir dir;
        Theme theme(dir.filePath(QStringLiteral("theme")));
        FakeFilePicker picker;
        FakePrinter printer;
        Backend backend(&theme, &picker, &printer,
                        dir.filePath(QStringLiteral("state")),
                        dir.filePath(QStringLiteral("config")));
        QTextDocument doc;
        backend.attachQTextDocument(&doc);

        doc.setPlainText(QStringLiteral("Fear and loathing"));
        QCOMPARE(backend.wordCount(), 3);

        backend.applyBold(0, 4);
        QCOMPARE(backend.documentText(), QStringLiteral("**Fear** and loathing"));
        QVERIFY(doc.isUndoAvailable());
        doc.undo();
        QCOMPARE(backend.documentText(), QStringLiteral("Fear and loathing"));
        doc.redo();
        QCOMPARE(backend.documentText(), QStringLiteral("**Fear** and loathing"));

        // Find and Replace All.
        doc.setPlainText(QStringLiteral("one two one three one"));
        const QVariantMap found = backend.setSearchQuery(QStringLiteral("one"), 0);
        QCOMPARE(found.value(QStringLiteral("found")).toBool(), true);
        QCOMPARE(backend.searchMatchCount(), 3);
        QCOMPARE(backend.replaceAll(QStringLiteral("ONE")), 3);
        QCOMPARE(backend.documentText(), QStringLiteral("ONE two ONE three ONE"));

        // Fake portal save & print.
        const QString savedPath = dir.filePath(QStringLiteral("out.md"));
        picker.nextSaveUrl = QUrl::fromLocalFile(savedPath);
        backend.save();
        QCOMPARE(picker.saveCalls, 1);
        QCOMPARE(DocumentStore::readFile(savedPath).text, QStringLiteral("ONE two ONE three ONE"));
        QVERIFY(!backend.modified());

        backend.printDocument();
        QCOMPARE(printer.calls, 1);
        QCOMPARE(printer.lastText, QStringLiteral("ONE two ONE three ONE"));
    }

    void portalPrinterRendersValidPdf() {
        QBuffer buf;
        QVERIFY(buf.open(QIODevice::WriteOnly));
        QVERIFY(PortalPrinter::renderPdf(QStringLiteral("# Gonzo\n\nPrintable source.\n"), &buf));
        QVERIFY(buf.data().startsWith("%PDF-"));
        QVERIFY(buf.data().size() > 500);
    }

    // --- SpellChecker with temporary Hunspell fixture ---

    void spellCheckerFlagsTyposAndSkipsCodeAndLinkDestinations() {
        QTemporaryDir dir;
        QVERIFY(writeBytes(dir.filePath(QStringLiteral("en_US.aff")), "SET UTF-8\n"));
        QVERIFY(writeBytes(dir.filePath(QStringLiteral("en_US.dic")), "4\nhello\nworld\nstory\ntext\n"));

        SpellChecker checker(QStringLiteral("en_US"), {dir.path()});
        QVERIFY(checker.isAvailable());
        QCOMPARE(checker.dictionaryName(), QStringLiteral("en_US"));

        const QString para = QStringLiteral("hello wrld `wrld` [story](https://wrld.example/bad)");
        const QList<TextRange> bad = checker.misspelledRanges(para);
        QCOMPARE(bad.size(), 1);
        QCOMPARE(para.mid(bad.first().start, bad.first().length), QStringLiteral("wrld"));
        QVERIFY(checker.suggestions(QStringLiteral("wrld")).contains(QStringLiteral("world")));

        checker.ignoreWord(QStringLiteral("wrld"));
        QVERIFY(checker.misspelledRanges(para).isEmpty());

        // Missing dictionary directory reports unavailable cleanly.
        SpellChecker missing(QStringLiteral("xx_XX"), {dir.filePath(QStringLiteral("empty"))});
        QVERIFY(!missing.isAvailable());
    }

    // --- KineticScroller physics ---

    void kineticScrollerMouseWheelAndTouchpadPhysics() {
        KineticScroller scroller;
        QVERIFY(!scroller.isActive());

        // 1. Mouse wheel notch starts curve and settles smoothly at target.
        qreal y = scroller.handleWheel(0.0, 2000.0, 0.0, -120.0, Qt::NoScrollPhase, 120.0, 1.0);
        QCOMPARE(y, 0.0);
        QVERIFY(scroller.isActive());

        qreal prev = y;
        for (int i = 0; i < 40 && scroller.isActive(); ++i) {
            y = scroller.advance(0.016, y, 2000.0, 1.0);
            QVERIFY(y >= prev);
            prev = y;
        }
        QVERIFY(!scroller.isActive());
        QCOMPARE(y, 120.0);

        // 2. Rapid wheel flicks retarget forward without backwards jitter.
        scroller.handleWheel(120.0, 2000.0, 0.0, -120.0, Qt::NoScrollPhase, 120.0, 1.0);
        y = scroller.advance(0.032, 120.0, 2000.0, 1.0);
        scroller.handleWheel(y, 2000.0, 0.0, -120.0, Qt::NoScrollPhase, 120.0, 1.0);
        qreal next = scroller.advance(0.016, y, 2000.0, 1.0);
        QVERIFY(next >= y);
        while (scroller.isActive())
            next = scroller.advance(0.016, next, 2000.0, 1.0);
        QCOMPARE(next, 360.0);

        // 3. Touchpad applies delta on ScrollBegin, tracks 1:1 on ScrollUpdate,
        // preserves flick momentum across a tiny lift-off frame, and coasts generously.
        y = scroller.handleWheel(200.0, 5000.0, -24.0, 0.0, Qt::ScrollBegin, 120.0, 1.0, 100);
        QCOMPARE(y, 224.0);
        y = scroller.handleWheel(y, 5000.0, -32.0, 0.0, Qt::ScrollUpdate, 120.0, 1.0, 116);
        QCOMPARE(y, 256.0);
        y = scroller.handleWheel(y, 5000.0, -30.0, 0.0, Qt::ScrollUpdate, 120.0, 1.0, 132);
        QCOMPARE(y, 286.0);
        // Tiny trailing lift-off frame must not kill the flick momentum.
        y = scroller.handleWheel(y, 5000.0, -2.0, 0.0, Qt::ScrollUpdate, 120.0, 1.0, 148);
        QCOMPARE(y, 288.0);
        QVERIFY(!scroller.isActive());
        scroller.handleWheel(y, 5000.0, 0.0, 0.0, Qt::ScrollEnd, 120.0, 1.0, 160);
        QVERIFY(scroller.isActive());
        qreal coasted = y;
        for (int i = 0; i < 300 && scroller.isActive(); ++i)
            coasted = scroller.advance(0.016, coasted, 5000.0, 1.0);
        QVERIFY(coasted - y > 400.0);
        QVERIFY(!scroller.isActive());

        // 3b. Holding fingers still on the pad (>110ms) before lifting does not coast.
        y = scroller.handleWheel(1000.0, 5000.0, -30.0, 0.0, Qt::ScrollBegin, 120.0, 1.0, 500);
        scroller.handleWheel(y, 5000.0, 0.0, 0.0, Qt::ScrollEnd, 120.0, 1.0, 680);
        QVERIFY(!scroller.isActive());

        // 4. Compositor ScrollMomentum cancels synthetic coast and applies 1:1.
        y = scroller.handleWheel(500.0, 5000.0, -40.0, 0.0, Qt::ScrollBegin, 120.0, 1.0, 1000);
        scroller.handleWheel(y, 5000.0, 0.0, 0.0, Qt::ScrollEnd, 120.0, 1.0, 1016);
        QVERIFY(scroller.isActive());
        y = scroller.handleWheel(y, 5000.0, -15.0, 0.0, Qt::ScrollMomentum, 120.0, 1.0, 1024);
        QCOMPARE(y, 555.0);
        QVERIFY(!scroller.isActive());

        // 5. Cancel stops immediately.
        scroller.handleWheel(555.0, 2000.0, 0.0, -120.0, Qt::NoScrollPhase, 120.0, 1.0);
        QVERIFY(scroller.isActive());
        scroller.cancel();
        QVERIFY(!scroller.isActive());
    }

    // --- Omarchy Theme, Translator, CLI & Offscreen QML ---

    void themeFollowsColorsTomlAndHyprlandRounding() {
        QTemporaryDir dir;
        Theme fallback(dir.path());
        QCOMPARE(fallback.accent(), QStringLiteral("#FFD60A"));
        QCOMPARE(fallback.accentForeground(), QStringLiteral("black"));
        QVERIFY(fallback.darkMode());

        QDir(dir.path()).mkpath(QStringLiteral("theme"));
        QVERIFY(writeBytes(dir.filePath(QStringLiteral("theme/colors.toml")),
                           "mode = \"light\"\nbackground = \"#f7f5ee\"\nforeground = \"#1a1a1a\"\naccent = \"#112233\"\n"));
        Theme lightTheme(dir.path());
        QVERIFY(!lightTheme.darkMode());
        QCOMPARE(lightTheme.background(), QStringLiteral("#f7f5ee"));
        QCOMPARE(lightTheme.accent(), QStringLiteral("#112233"));
        QCOMPARE(lightTheme.accentForeground(), QStringLiteral("white"));

        const QString resp = dir.filePath(QStringLiteral("resp.json"));
        const QString cmd = dir.filePath(QStringLiteral("hyprctl"));
        QVERIFY(writeBytes(cmd, "#!/bin/sh\ncat '" + resp.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(cmd, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(writeBytes(resp, "{\"int\":0}"));
        Theme rounded(dir.path(), nullptr, cmd);
        QCOMPARE(rounded.radius(), 0);
        QVERIFY(writeBytes(resp, "{\"int\":8}"));
        QTRY_COMPARE(rounded.radius(), 8);
    }

    void translatorFollowsLanguageAndPlurals() {
        QTemporaryDir dir;
        Translator en(dir.filePath(QStringLiteral("language")), QStringLiteral(":/i18n"));
        QCOMPARE(en.language(), QStringLiteral("en"));
        QCOMPARE(en.translate("Main", "%n words", nullptr, 1), QStringLiteral("%n word"));
        QCOMPARE(en.translate("Main", "%n words", nullptr, 5), QStringLiteral("%n words"));

        QVERIFY(writeBytes(dir.filePath(QStringLiteral("language")), "ru\n"));
        Translator ru(dir.filePath(QStringLiteral("language")), QStringLiteral(":/i18n"));
        QCOMPARE(ru.language(), QStringLiteral("ru"));
        QCOMPARE(ru.translate("Main", "%n words", nullptr, 21), QStringLiteral("%n слово"));
        QCOMPARE(ru.translate("Main", "%n words", nullptr, 22), QStringLiteral("%n слова"));
        QCOMPARE(ru.translate("Main", "%n words", nullptr, 25), QStringLiteral("%n слов"));
        QCOMPARE(Translator::pluralCategory(QStringLiteral("ar"), 11), QStringLiteral("many"));
    }

    void cliStatsCheckAndSkillInstall() {
        QTemporaryDir dir;
        const QString doc = dir.filePath(QStringLiteral("note.md"));
        QVERIFY(writeBytes(doc, "# Story\n\nHello [world](https://example.com)!\n"));

        QString out, err;
        QCOMPARE(Cli::run({QStringLiteral("gonzo-writer"), QStringLiteral("stats"), QStringLiteral("--json"), doc}, &out, &err), 0);
        const QJsonObject stats = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(stats.value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(stats.value(QStringLiteral("words")).toInt(), 3);
        QCOMPARE(stats.value(QStringLiteral("headings")).toInt(), 1);
        QCOMPARE(stats.value(QStringLiteral("links")).toInt(), 1);

        out.clear(); err.clear();
        QCOMPARE(Cli::run({QStringLiteral("gonzo-writer"), QStringLiteral("check"), QStringLiteral("--json"), doc}, &out, &err), 0);
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object().value(QStringLiteral("ok")).toBool(), true);

        out.clear(); err.clear();
        const QString paraDoc = dir.filePath(QStringLiteral("paras.md"));
        QVERIFY(writeBytes(paraDoc, "line one\nline two\n\nsecond paragraph\r\ncontinued\n\n\nthird\n"));
        QCOMPARE(Cli::run({QStringLiteral("gonzo-writer"), QStringLiteral("stats"), QStringLiteral("--json"), paraDoc}, &out, &err), 0);
        const QJsonObject paraStats = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(paraStats.value(QStringLiteral("paragraphs")).toInt(), 3);

        const QString fakeHome = dir.filePath(QStringLiteral("home"));
        QDir().mkpath(fakeHome + QStringLiteral("/.claude"));
        out.clear(); err.clear();
        QCOMPARE(Cli::run({QStringLiteral("gonzo-writer"), QStringLiteral("skill"), QStringLiteral("install")}, &out, &err, fakeHome), 0);
        QVERIFY(QFileInfo::exists(fakeHome + QStringLiteral("/.agents/skills/gonzo-writer/SKILL.md")));
        QVERIFY(QFileInfo(fakeHome + QStringLiteral("/.claude/skills/gonzo-writer")).isSymLink());
    }

    // --- Review regression tests ---

    void openWhileModifiedPromptsAndPreservesDraft() {
        QTemporaryDir dir;
        const QString stateDir = dir.filePath(QStringLiteral("state"));
        const QString other = dir.filePath(QStringLiteral("other.md"));
        QVERIFY(writeBytes(other, "Other file"));

        Theme theme(dir.filePath(QStringLiteral("theme")));
        FakeFilePicker picker;
        Backend backend(&theme, &picker, nullptr, stateDir, dir.filePath(QStringLiteral("config")));
        QTextDocument doc;
        backend.attachQTextDocument(&doc);

        doc.setPlainText(QStringLiteral("Unsaved draft"));
        backend.flushRecoveryNow();
        QVERIFY(backend.modified());

        QSignalSpy unsavedSpy(&backend, &Backend::unsavedChangesRequested);
        QVERIFY(!backend.openFile(other));
        QCOMPARE(unsavedSpy.count(), 1);
        QCOMPARE(backend.documentText(), QStringLiteral("Unsaved draft"));
        QVERIFY(!QDir(stateDir + QStringLiteral("/recovery")).entryList({QStringLiteral("*.md")}, QDir::Files).isEmpty());

        // Cancelling keeps edits and recovery draft intact.
        backend.cancelPendingAction();
        QCOMPARE(backend.documentText(), QStringLiteral("Unsaved draft"));

        // Confirming discard opens the requested file cleanly.
        QVERIFY(!backend.openFile(other));
        backend.confirmDiscardAndProceed();
        QCOMPARE(backend.documentText(), QStringLiteral("Other file"));
        QVERIFY(!backend.modified());
    }

    void crlfFileReplaceAllAndNbspPreservation() {
        QTemporaryDir dir;
        const QString crlfFile = dir.filePath(QStringLiteral("win.md"));
        QVERIFY(writeBytes(crlfFile, "alpha\r\nbeta\r\nalpha\u00A0gamma\r\n"));

        Theme theme(dir.filePath(QStringLiteral("theme")));
        Backend backend(&theme, nullptr, nullptr,
                        dir.filePath(QStringLiteral("state")),
                        dir.filePath(QStringLiteral("config")));
        QTextDocument doc;
        backend.attachQTextDocument(&doc);

        QVERIFY(backend.openFile(crlfFile));
        const QVariantMap found = backend.setSearchQuery(QStringLiteral("alpha"), 0);
        QCOMPARE(found.value(QStringLiteral("found")).toBool(), true);
        QCOMPARE(backend.searchMatchCount(), 2);
        QCOMPARE(backend.replaceAll(QStringLiteral("OMEGA")), 2);
        QCOMPARE(backend.documentText(), QStringLiteral("OMEGA\nbeta\nOMEGA\u00A0gamma\n"));

        // Saving preserves CRLF line endings and NBSP.
        QVERIFY(backend.save());
        QFile raw(crlfFile);
        QVERIFY(raw.open(QIODevice::ReadOnly));
        QCOMPARE(raw.readAll(), QByteArray("OMEGA\r\nbeta\r\nOMEGA\xC2\xA0gamma\r\n"));

        // Fallback bulk path (setPlainText) also preserves NBSP.
        doc.setPlainText(QStringLiteral("keep\u00A0nbsp"));
        QCOMPARE(backend.documentText(), QStringLiteral("keep\u00A0nbsp"));
    }

    void zoomPreservesUndoStackAndSinglePassLoadHighlight() {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("doc.md"));
        QVERIFY(writeBytes(file, "# Heading\n\nLine one\n\nLine two\n"));

        Theme theme(dir.filePath(QStringLiteral("theme")));
        Backend backend(&theme, nullptr, nullptr,
                        dir.filePath(QStringLiteral("state")),
                        dir.filePath(QStringLiteral("config")));
        QTextDocument doc;
        backend.attachQTextDocument(&doc);

        backend.highlighter()->resetHighlightBlockCount();
        QVERIFY(backend.openFile(file));
        QCoreApplication::processEvents();
        const int blocks = doc.blockCount();
        // Load must highlight the document only once (not 3x).
        QCOMPARE(backend.highlighter()->highlightBlockCount(), blocks);

        // Apply an edit, zoom in/out, and confirm Undo still reverts it.
        backend.applyBold(11, 19);
        QVERIFY(backend.documentText().contains(QStringLiteral("**Line one**")));
        backend.highlighter()->resetHighlightBlockCount();
        backend.increaseTextSize();
        QCOMPARE(backend.highlighter()->highlightBlockCount(), blocks);
        backend.decreaseTextSize();
        QVERIFY(doc.isUndoAvailable());
        doc.undo();
        QVERIFY(!backend.documentText().contains(QStringLiteral("**Line one**")));
    }

    void findNextAndEditsOnlyRehighlightAffectedBlocks() {
        QTemporaryDir dir;
        Theme theme(dir.filePath(QStringLiteral("theme")));
        Backend backend(&theme, nullptr, nullptr,
                        dir.filePath(QStringLiteral("state")),
                        dir.filePath(QStringLiteral("config")));
        QTextDocument doc;
        backend.attachQTextDocument(&doc);

        QString body;
        for (int i = 0; i < 40; ++i)
            body += QStringLiteral("Paragraph %1 with target word.\n").arg(i);
        doc.setPlainText(body);

        backend.setSearchQuery(QStringLiteral("target"), 0);
        QCOMPARE(backend.searchMatchCount(), 40);

        backend.highlighter()->resetHighlightBlockCount();
        backend.findNext();
        // Stepping to next match only re-highlights the old/new match blocks
        // (and Qt's adjacent state check), staying O(1) instead of all 40 blocks.
        QVERIFY2(backend.highlighter()->highlightBlockCount() <= 4,
                 qPrintable(QStringLiteral("Rehighlighted %1 blocks").arg(backend.highlighter()->highlightBlockCount())));
    }

    void unreadableRecoveryDraftIsNotDeleted() {
        QTemporaryDir dir;
        const QString stateDir = dir.filePath(QStringLiteral("state"));
        const QString recDir = stateDir + QStringLiteral("/recovery");
        QDir().mkpath(recDir);
        const QString badDraft = recDir + QStringLiteral("/draft1.md");
        const QString badMeta = recDir + QStringLiteral("/draft1.json");
        // Non-UTF-8 bytes cause readFile to fail; draft must be preserved, not deleted.
        QVERIFY(writeBytes(badDraft, QByteArray("\xFF\xFE\x01\x02")));
        QVERIFY(writeBytes(badMeta, "{}"));

        DocumentStore store(stateDir);
        QVERIFY(!store.takeRecoverableDraft().has_value());
        QVERIFY(QFileInfo::exists(badDraft));
        QVERIFY(QFileInfo::exists(badMeta));
    }

    void spellCheckerHandlesNonUtf8DictionaryEncoding() {
        QTemporaryDir dir;
        QVERIFY(writeBytes(dir.filePath(QStringLiteral("fr_FR.aff")), "SET ISO8859-1\n"));
        // "café" in ISO-8859-1 is "caf\xE9"
        QVERIFY(writeBytes(dir.filePath(QStringLiteral("fr_FR.dic")), QByteArray("1\ncaf\xE9\n")));

        SpellChecker checker(QStringLiteral("fr_FR"), {dir.path()});
        QVERIFY(checker.isAvailable());
        QVERIFY(!checker.isMisspelled(QStringLiteral("café")));
        QVERIFY(checker.isMisspelled(QStringLiteral("cafx")));
        QVERIFY(checker.suggestions(QStringLiteral("cafx")).contains(QStringLiteral("café")));
    }

    void cancelledSaveDoesNotQuitOnSubsequentSave() {
        QTemporaryDir dir;
        Theme theme(dir.filePath(QStringLiteral("theme")));
        FakeFilePicker picker;
        Backend backend(&theme, &picker, nullptr,
                        dir.filePath(QStringLiteral("state")),
                        dir.filePath(QStringLiteral("config")));
        QTextDocument doc;
        backend.attachQTextDocument(&doc);
        doc.setPlainText(QStringLiteral("Draft"));

        QSignalSpy closeSpy(&backend, &Backend::closeApproved);
        backend.requestClose();
        // User chooses Save in Unsaved Changes dialog, then cancels the picker.
        backend.confirmSaveAndProceed();
        emit picker.cancelled(true);

        // A later explicit Save As must NOT close the app.
        picker.nextSaveUrl = QUrl::fromLocalFile(dir.filePath(QStringLiteral("saved.md")));
        backend.save();
        QCOMPARE(closeSpy.count(), 0);
    }

    void qmlLoadsCleanlyOffscreen() {
        QQuickStyle::setStyle(QStringLiteral("Material"));
        QTemporaryDir dir;
        Translator translator(dir.filePath(QStringLiteral("language")));
        Theme theme(dir.filePath(QStringLiteral("theme")));
        FakeFilePicker picker;
        FakePrinter printer;
        KineticScroller scroller;
        Backend backend(&theme, &picker, &printer,
                        dir.filePath(QStringLiteral("state")),
                        dir.filePath(QStringLiteral("config")));

        QQmlApplicationEngine engine;
        QList<QQmlError> warnings;
        QObject::connect(&engine, &QQmlApplicationEngine::warnings, &engine,
                         [&warnings](const QList<QQmlError> &list) { warnings.append(list); });
        engine.rootContext()->setContextProperty(QStringLiteral("translator"), &translator);
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        engine.rootContext()->setContextProperty(QStringLiteral("scroller"), &scroller);
        engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));

        QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to load");
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));
        auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(win != nullptr);
        QVERIFY(backend.textDocument() != nullptr);
        win->close();
    }
};

QTEST_MAIN(GonzoTests)
#include "gonzo_tests.moc"
