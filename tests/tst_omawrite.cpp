#include <QtTest>
#include <QClipboard>
#include <QFont>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>

#include "backend.h"
#include "markdownhighlighter.h"

class OmawriteTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_settingsDirectory.isValid());
        QQuickStyle::setStyle(QStringLiteral("Material"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_settingsDirectory.path());
    }

    void countsWords() {
        QCOMPARE(Backend::countWords(QStringLiteral("one two-three don't 42")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("你好 世界")), 2);
        QCOMPARE(Backend::countWords(QString()), 0);
    }

    void normalizesLinks() {
        QCOMPARE(Backend::normalizedLinkUrl(QStringLiteral("www.example.com/path")),
                 QStringLiteral("https://www.example.com/path"));
        QCOMPARE(Backend::normalizedLinkUrl(QStringLiteral("mailto:writer@example.com")),
                 QStringLiteral("mailto:writer@example.com"));
        QVERIFY(Backend::normalizedLinkUrl(QStringLiteral("example.com")).isEmpty());
        QVERIFY(Backend::normalizedLinkUrl(QStringLiteral("file:///tmp/private")).isEmpty());
    }

    void suggestsSafeNames() {
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("My first draft\nBody")),
                 QStringLiteral("My first draft.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("A/B")), QStringLiteral("A-B.md"));
        QCOMPARE(Backend::suggestedFileName(QString()), QStringLiteral("Untitled.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("Already.md")),
                 QStringLiteral("Already.md"));
    }

    void findsInlineMarkdownRanges() {
        const auto markup = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**bold** and *italic* and [site](https://example.com)"));
        QCOMPARE(markup.size(), 3);
        QCOMPARE(markup.at(0).content.start, 2);
        QCOMPARE(markup.at(0).content.length, 4);
        QCOMPARE(markup.at(2).content.length, 4);
        QCOMPARE(markup.at(2).markers[0].length, 1);
    }

    void loadsCurrentOmarchyTheme() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());

        const QByteArray originalHome = qgetenv("HOME");
        struct HomeRestorer {
            QByteArray value;
            ~HomeRestorer() { qputenv("HOME", value); }
        } restoreHome{originalHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));

        const QString themeDirectory = homeDirectory.path()
            + QStringLiteral("/.local/state/omarchy/current/theme");
        QVERIFY(QDir().mkpath(themeDirectory));

        QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
        QVERIFY(colorsFile.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray palette(
            "mode = \"light\"\n"
            "accent = \"#112233\"\n"
            "selection = \"#445566\"\n"
            "background = \"#fefefe\"\n"
            "foreground = \"#101010\"\n");
        QCOMPARE(colorsFile.write(palette), qint64(palette.size()));
        colorsFile.close();

        Backend backend;
        QCOMPARE(backend.themeBackground(), QStringLiteral("#fefefe"));
        QCOMPARE(backend.themeForeground(), QStringLiteral("#101010"));
        QCOMPARE(backend.themeAccent(), QStringLiteral("#112233"));
        QCOMPARE(backend.themeSelection(), QStringLiteral("#445566"));
        QVERIFY(!backend.darkMode());
    }

    void ignoresFileWatcherEventsForSavedContents() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("first-save.md"));
        Backend backend;
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        backend.saveAs(QUrl::fromLocalFile(path));
        QVERIFY(QFileInfo::exists(path));

        QFile sameContents(path);
        QVERIFY(sameContents.open(QIODevice::WriteOnly | QIODevice::Truncate));
        sameContents.close();
        QTest::qWait(100);
        QCOMPARE(externalChangeSpy.count(), 0);

        QFile changedContents(path);
        QVERIFY(changedContents.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(changedContents.write("changed elsewhere"), qint64(17));
        changedContents.close();
        QTRY_COMPARE(externalChangeSpy.count(), 1);
    }

    void keepsCursorAndSelectionStableAcrossInsertions() {
        const QString mutationsPath = QFINDTESTDATA("../src/EditorMutations.js");
        QVERIFY(!mutationsPath.isEmpty());

        QQmlEngine engine;
        QQmlComponent component(&engine);
        const QByteArray harness = R"QML(
            import QtQuick
            import "EditorMutations.js" as EditorMutations

            TextEdit {
                property string insertionText
                property int insertionCursor
                property string wrappedText
                property int wrappedSelectionStart
                property int wrappedSelectionEnd

                Component.onCompleted: {
                    text = "alpha omega";
                    cursorPosition = 5;
                    EditorMutations.replaceRange(this, 5, 5, "one\r\ntwo");
                    insertionText = text;
                    insertionCursor = cursorPosition;

                    text = "alpha beta omega";
                    select(6, 10);
                    EditorMutations.replaceRange(this, selectionStart, selectionEnd,
                                                 "**beta**", 2, 6);
                    wrappedText = text;
                    wrappedSelectionStart = selectionStart;
                    wrappedSelectionEnd = selectionEnd;
                }
            }
        )QML";
        const QUrl harnessUrl = QUrl::fromLocalFile(
            QFileInfo(mutationsPath).absolutePath() + QStringLiteral("/MutationHarness.qml"));
        component.setData(harness, harnessUrl);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));

        QCOMPARE(editor->property("insertionText").toString(),
                 QStringLiteral("alphaone\ntwo omega"));
        QCOMPARE(editor->property("insertionCursor").toInt(), 12);
        QCOMPARE(editor->property("wrappedText").toString(),
                 QStringLiteral("alpha **beta** omega"));
        QCOMPARE(editor->property("wrappedSelectionStart").toInt(), 8);
        QCOMPARE(editor->property("wrappedSelectionEnd").toInt(), 12);
    }

    void movesAndEditsWithVimKeys() {
        QScopedPointer<QObject> editor(createVimHarness());
        QVERIFY(!editor.isNull());

        // Motions stay on a character, never on the line break past it.
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta\ngamma"), 0,
                        QStringLiteral("$")).cursor, 9);
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta"), 0,
                        QStringLiteral("wl")).cursor, 7);
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta"), 9,
                        QStringLiteral("b")).cursor, 6);
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha, beta"), 0,
                        QStringLiteral("w")).cursor, 5);
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha, beta"), 0,
                        QStringLiteral("W")).cursor, 7);
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\nthree"), 0,
                        QStringLiteral("G")).cursor, 8);
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\nthree"), 10,
                        QStringLiteral("gg")).cursor, 0);
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\nthree"), 0,
                        QStringLiteral("2G")).cursor, 4);
        QCOMPARE(runVim(editor.data(), QStringLiteral("hello there"), 0,
                        QStringLiteral("fh")).cursor, 7);
        QCOMPARE(runVim(editor.data(), QStringLiteral("hello there"), 0,
                        QStringLiteral("th")).cursor, 6);
        QCOMPARE(runVim(editor.data(), QStringLiteral("hello there"), 10,
                        QStringLiteral("Fh")).cursor, 7);

        // A repeated t walks on instead of re-finding the target it already
        // stopped short of, while a fresh t does stop short of an adjacent one.
        QCOMPARE(runVim(editor.data(), QStringLiteral("a.b.c.d"), 0,
                        QStringLiteral("t.")).cursor, 0);
        QCOMPARE(runVim(editor.data(), QStringLiteral("a.b.c.d"), 0,
                        QStringLiteral("t.;")).cursor, 2);
        QCOMPARE(runVim(editor.data(), QStringLiteral("a.b.c.d"), 0,
                        QStringLiteral("t.;;")).cursor, 4);
        QCOMPARE(runVim(editor.data(), QStringLiteral("a.b.c.d"), 0,
                        QStringLiteral("f.;")).cursor, 3);

        // Astral characters step and delete whole, never half a pair.
        const QString emoji = QStringLiteral("a\U0001F600b");
        QCOMPARE(runVim(editor.data(), emoji, 0, QStringLiteral("l")).cursor, 1);
        QCOMPARE(runVim(editor.data(), emoji, 0, QStringLiteral("ll")).cursor, 3);
        QCOMPARE(runVim(editor.data(), emoji, 3, QStringLiteral("h")).cursor, 1);
        QCOMPARE(runVim(editor.data(), emoji, 1, QStringLiteral("x")).text,
                 QStringLiteral("ab"));
        QCOMPARE(runVim(editor.data(), emoji, 1, QStringLiteral("rz")).text,
                 QStringLiteral("azb"));

        // j and k hold the column they started from across a short line.
        QCOMPARE(runVim(editor.data(), QStringLiteral("abcdef\nx\nabcdef"), 4,
                        QStringLiteral("jj")).cursor, 13);

        // Sentences run to the first word after the punctuation, and stop at
        // the end of the paragraph rather than running into the next one.
        const QString prose = QStringLiteral("One thing. Two things! Three?\n\nNext para.");
        QCOMPARE(runVim(editor.data(), prose, 0, QStringLiteral(")")).cursor, 11);
        QCOMPARE(runVim(editor.data(), prose, 0, QStringLiteral("2)")).cursor, 23);
        QCOMPARE(runVim(editor.data(), prose, 23, QStringLiteral("(")).cursor, 11);
        QCOMPARE(runVim(editor.data(), prose, 0, QStringLiteral("3)")).cursor, 30);
        QCOMPARE(runVim(editor.data(), prose, 0, QStringLiteral("d)")).text,
                 QStringLiteral("Two things! Three?\n\nNext para."));

        // ge walks back to the end of the previous word, and takes the
        // caret's own character when an operator is waiting.
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta gamma"), 11,
                        QStringLiteral("ge")).cursor, 9);
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta gamma"), 11,
                        QStringLiteral("2ge")).cursor, 4);
        // From the t of "beta" back through the a of "alpha", both ends
        // taken, which leaves the trailing a of "beta" behind.
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta"), 8,
                        QStringLiteral("dge")).text, QStringLiteral("alpha"));

        const VimResult deleted = runVim(editor.data(), QStringLiteral("alpha beta gamma"), 0,
                                         QStringLiteral("dw"));
        QCOMPARE(deleted.text, QStringLiteral("beta gamma"));
        QCOMPARE(deleted.cursor, 0);

        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta"), 6,
                        QStringLiteral("d$")).text, QStringLiteral("alpha "));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\nthree"), 0,
                        QStringLiteral("2dd")).text, QStringLiteral("three"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo"), 5,
                        QStringLiteral("dd")).text, QStringLiteral("one"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("abcdef"), 1,
                        QStringLiteral("3x")).text, QStringLiteral("aef"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo"), 0,
                        QStringLiteral("J")).text, QStringLiteral("one two"));
        // Nothing below to join to, so the line is left as it is.
        QCOMPARE(runVim(editor.data(), QStringLiteral("one two"), 0,
                        QStringLiteral("J")).text, QStringLiteral("one two"));

        // Whole-line yank pastes below the line the caret is on.
        const VimResult pasted = runVim(editor.data(), QStringLiteral("one\ntwo"), 0,
                                        QStringLiteral("yyp"));
        QCOMPARE(pasted.text, QStringLiteral("one\none\ntwo"));
        QCOMPARE(pasted.cursor, 4);

        const VimResult swapped = runVim(editor.data(), QStringLiteral("ab"), 0,
                                         QStringLiteral("xp"));
        QCOMPARE(swapped.text, QStringLiteral("ba"));
        QCOMPARE(swapped.cursor, 1);
    }

    void changesTextAndRepeatsWithVimKeys() {
        QScopedPointer<QObject> editor(createVimHarness());
        QVERIFY(!editor.isNull());

        const VimResult changed = runVim(editor.data(), QStringLiteral("alpha beta"), 0,
                                         QStringLiteral("cwomega<Esc>"));
        QCOMPARE(changed.text, QStringLiteral("omega beta"));
        QCOMPARE(changed.mode, QStringLiteral("normal"));
        QCOMPARE(changed.cursor, 4);

        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha"), 0,
                        QStringLiteral("A!<Esc>")).text, QStringLiteral("alpha!"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha"), 2,
                        QStringLiteral("Inew <Esc>")).text, QStringLiteral("new alpha"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one"), 0,
                        QStringLiteral("otwo<Esc>")).text, QStringLiteral("one\ntwo"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("two"), 0,
                        QStringLiteral("Oone<Esc>")).text, QStringLiteral("one\ntwo"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("  - item"), 4,
                        QStringLiteral("ccnext<Esc>")).text, QStringLiteral("  next"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("cat"), 0,
                        QStringLiteral("rb")).text, QStringLiteral("bat"));

        // Visual mode operates on the highlighted characters, inclusive.
        const VimResult visual = runVim(editor.data(), QStringLiteral("alpha beta"), 0,
                                        QStringLiteral("vlld"));
        QCOMPARE(visual.text, QStringLiteral("ha beta"));
        QCOMPARE(visual.mode, QStringLiteral("normal"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\nthree"), 5,
                        QStringLiteral("Vd")).text, QStringLiteral("one\nthree"));

        // The dot command repeats the last change, typed text included.
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta gamma"), 0,
                        QStringLiteral("dw.")).text, QStringLiteral("gamma"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one two"), 0,
                        QStringLiteral("cwX<Esc>w.")).text, QStringLiteral("X X"));

        // Undo takes back the whole command, not the edits inside it.
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo"), 0,
                        QStringLiteral("ddu")).text, QStringLiteral("one\ntwo"));

        // Text objects take a span without a motion. iw stops at the word,
        // aw takes the space after it, and both work under any operator.
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta gamma"), 7,
                        QStringLiteral("diw")).text, QStringLiteral("alpha  gamma"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta gamma"), 7,
                        QStringLiteral("daw")).text, QStringLiteral("alpha gamma"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta"), 8,
                        QStringLiteral("ciwomega<Esc>")).text, QStringLiteral("alpha omega"));

        // The quote and bracket pairs, from anywhere inside them.
        QCOMPARE(runVim(editor.data(), QStringLiteral("say \"hello there\" now"), 9,
                        QStringLiteral("di\"")).text, QStringLiteral("say \"\" now"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("say \"hello\" now"), 6,
                        QStringLiteral("da\"")).text, QStringLiteral("say  now"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("f(a, g(b), c)"), 4,
                        QStringLiteral("di(")).text, QStringLiteral("f()"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("f(a, g(b), c)"), 7,
                        QStringLiteral("di(")).text, QStringLiteral("f(a, g(), c)"));

        // ip is linewise and stops at the blank line; ap takes the gap too.
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\n\nthree"), 4,
                        QStringLiteral("dip")).text, QStringLiteral("\nthree"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\n\nthree"), 4,
                        QStringLiteral("dap")).text, QStringLiteral("three"));

        // From visual mode the object becomes the selection.
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta gamma"), 7,
                        QStringLiteral("viwd")).text, QStringLiteral("alpha  gamma"));

        // An object that does not resolve leaves the document alone.
        QCOMPARE(runVim(editor.data(), QStringLiteral("no quotes here"), 3,
                        QStringLiteral("di\"")).text, QStringLiteral("no quotes here"));

        // Normal mode never lets a plain letter reach the document.
        const VimResult swallowed = runVim(editor.data(), QStringLiteral("text"), 0,
                                           QStringLiteral("zq"));
        QCOMPARE(swallowed.text, QStringLiteral("text"));

        // Insert mode hands every key back to the editor.
        QCOMPARE(editor->property("unhandled").toStringList(), QStringList());
        runVim(editor.data(), QStringLiteral("x"), 0, QStringLiteral("ia"));
        QCOMPARE(editor->property("unhandled").toStringList(), QStringList{QStringLiteral("a")});
    }

    void yanksAndPastesThroughNamedRegisters() {
        QScopedPointer<QObject> editor(createVimHarness());
        QVERIFY(!editor.isNull());

        auto clipboard = [&] { return editor->property("clipboardText").toString(); };

        // "a holds a line aside while the unnamed register moves on, and "ap
        // pastes what was put there rather than what was deleted since.
        const VimResult aside = runVim(editor.data(), QStringLiteral("one\ntwo\nthree"), 0,
                                       QStringLiteral("\"ayyjdd\"ap"));
        QCOMPARE(aside.text, QStringLiteral("one\nthree\none"));

        // A bare p still pastes the last thing taken, named or not.
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo"), 0,
                        QStringLiteral("\"ayyp")).text, QStringLiteral("one\none\ntwo"));

        // Nothing reaches the system clipboard unless a register asks it to.
        runVim(editor.data(), QStringLiteral("one\ntwo"), 0, QStringLiteral("yydd"));
        QCOMPARE(clipboard(), QString());

        // "+y does, and marks a linewise yank with the trailing newline that
        // is all a clipboard can carry.
        runVim(editor.data(), QStringLiteral("one\ntwo"), 0, QStringLiteral("\"+yy"));
        QCOMPARE(clipboard(), QStringLiteral("one\n"));
        runVim(editor.data(), QStringLiteral("alpha beta"), 0, QStringLiteral("\"+yw"));
        QCOMPARE(clipboard(), QStringLiteral("alpha "));

        // "+p reads it back, and the trailing newline makes it linewise again.
        editor->setProperty("clipboardText", QStringLiteral("carried\n"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo"), 0,
                        QStringLiteral("\"+p")).text, QStringLiteral("one\ncarried\ntwo"));
        editor->setProperty("clipboardText", QStringLiteral("carried"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("ab"), 0,
                        QStringLiteral("\"+p")).text, QStringLiteral("acarriedb"));

        // "* is the primary selection, a different register entirely.
        editor->setProperty("clipboardText", QString());
        runVim(editor.data(), QStringLiteral("one\ntwo"), 0, QStringLiteral("\"*yy"));
        QCOMPARE(editor->property("selectionText").toString(), QStringLiteral("one\n"));
        QCOMPARE(clipboard(), QString());

        // A delete into "b keeps the clipboard out of it.
        editor->setProperty("clipboardText", QStringLiteral("untouched"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta"), 0,
                        QStringLiteral("\"bdw\"bP")).text, QStringLiteral("alpha beta"));
        QCOMPARE(clipboard(), QStringLiteral("untouched"));

        // The pending register shows in the footer while it waits.
        QMetaObject::invokeMethod(editor.data(), "reset",
                                  Q_ARG(QVariant, QStringLiteral("text")), Q_ARG(QVariant, 0));
        QMetaObject::invokeMethod(editor.data(), "feed", Q_ARG(QVariant, QStringLiteral("\"a2")));
        QVariant status;
        QMetaObject::invokeMethod(editor.data(), "status", Q_RETURN_ARG(QVariant, status));
        QCOMPARE(status.toString(), QStringLiteral("NORMAL \"a2"));
    }

    void runsExCommands() {
        QScopedPointer<QObject> editor(createVimHarness());
        QVERIFY(!editor.isNull());

        const QString document = QStringLiteral("one fish\ntwo fish\nred fish\nblue fish");

        // A bare range jumps; the caret lands on the first non-blank.
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":3")).cursor, 18);
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":$")).cursor, 27);
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":99")).cursor, 27);

        // Substitute defaults to the current line, once per line.
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":s/fish/cat/")).text,
                 QStringLiteral("one cat\ntwo fish\nred fish\nblue fish"));
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":%s/fish/cat/")).text,
                 QStringLiteral("one cat\ntwo cat\nred cat\nblue cat"));
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":2,3s/fish/cat/")).text,
                 QStringLiteral("one fish\ntwo cat\nred cat\nblue fish"));

        // g replaces every match on the line, i ignores case.
        QCOMPARE(runEx(editor.data(), QStringLiteral("a a a"), 0,
                       QStringLiteral(":s/a/b/")).text, QStringLiteral("b a a"));
        QCOMPARE(runEx(editor.data(), QStringLiteral("a a a"), 0,
                       QStringLiteral(":s/a/b/g")).text, QStringLiteral("b b b"));
        QCOMPARE(runEx(editor.data(), QStringLiteral("Fish fish"), 0,
                       QStringLiteral(":s/fish/cat/gi")).text, QStringLiteral("cat cat"));

        // & is the whole match, \1 a captured group, and a separator can be
        // escaped or swapped for one that does not collide with the text.
        QCOMPARE(runEx(editor.data(), QStringLiteral("hello"), 0,
                       QStringLiteral(":s/hello/[&]/")).text, QStringLiteral("[hello]"));
        QCOMPARE(runEx(editor.data(), QStringLiteral("Doe, John"), 0,
                       QStringLiteral(":s/(\\w+), (\\w+)/\\2 \\1/")).text,
                 QStringLiteral("John Doe"));
        QCOMPARE(runEx(editor.data(), QStringLiteral("a/b"), 0,
                       QStringLiteral(":s#/#-#")).text, QStringLiteral("a-b"));

        // Reported outcomes, including the ones that change nothing.
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":%s/fish/cat/")).message,
                 QStringLiteral("4 substitutions on 4 lines"));
        const VimResult missing = runEx(editor.data(), document, 0,
                                        QStringLiteral(":s/whale/cat/"));
        QVERIFY(!missing.ok);
        QCOMPARE(missing.message, QStringLiteral("Pattern not found: whale"));
        QCOMPARE(missing.text, document);

        const VimResult unknown = runEx(editor.data(), document, 0, QStringLiteral(":frobnicate"));
        QVERIFY(!unknown.ok);
        QCOMPARE(unknown.message, QStringLiteral("Not an editor command: frobnicate"));

        const VimResult broken = runEx(editor.data(), document, 0, QStringLiteral(":s/(unclosed/x/"));
        QVERIFY(!broken.ok);
        QCOMPARE(broken.text, document);

        // :d takes whole lines, and leaves them in the register for p.
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":2d")).text,
                 QStringLiteral("one fish\nred fish\nblue fish"));
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":2,3d")).text,
                 QStringLiteral("one fish\nblue fish"));

        // An empty pattern reuses the last one.
        QMetaObject::invokeMethod(editor.data(), "reset", Q_ARG(QVariant, document), Q_ARG(QVariant, 0));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":s/fish/cat/")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":2s//dog/")));
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("one cat\ntwo dog\nred fish\nblue fish"));

        // Pressing : in visual mode opens the command line prefilled with the
        // range covering the selected lines.
        QMetaObject::invokeMethod(editor.data(), "reset", Q_ARG(QVariant, document), Q_ARG(QVariant, 9));
        QMetaObject::invokeMethod(editor.data(), "feed", Q_ARG(QVariant, QStringLiteral("Vj:")));
        QCOMPARE(editor->property("calls").toStringList(),
                 QStringList{QStringLiteral("commandLine:'<,'>")});
        QCOMPARE(readVim(editor.data()).mode, QStringLiteral("normal"));
        QMetaObject::invokeMethod(editor.data(), "ex",
                                  Q_ARG(QVariant, QStringLiteral(":'<,'>s/fish/cat/")));
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("one fish\ntwo cat\nred cat\nblue fish"));
    }

    void callsTheApplicationForFileExCommands() {
        QScopedPointer<QObject> editor(createVimHarness());
        QVERIFY(!editor.isNull());

        QMetaObject::invokeMethod(editor.data(), "reset", Q_ARG(QVariant, QString()), Q_ARG(QVariant, 0));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":w")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":w draft.md")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":wq")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":q")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":q!")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":e notes.md")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":noh")));
        // Abbreviations reach the same commands as the spelled-out names.
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":write")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":x")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":quit")));

        QCOMPARE(editor->property("calls").toStringList(),
                 QStringList({QStringLiteral("save"),
                              QStringLiteral("saveAs:draft.md"),
                              QStringLiteral("saveAndQuit"),
                              QStringLiteral("quit:false"),
                              QStringLiteral("quit:true"),
                              QStringLiteral("open:notes.md:false"),
                              QStringLiteral("clearSearch"),
                              QStringLiteral("save"),
                              QStringLiteral("saveAndQuit"),
                              QStringLiteral("quit:false")}));
    }

    void resolvesPathsTypedOnTheCommandLine() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        Backend backend;
        // With no file open, a bare name lands in the home directory.
        QCOMPARE(backend.resolvePath(QStringLiteral("draft.md")).toLocalFile(),
                 QDir::homePath() + QStringLiteral("/draft.md"));
        QCOMPARE(backend.resolvePath(QStringLiteral("~/notes/draft.md")).toLocalFile(),
                 QDir::homePath() + QStringLiteral("/notes/draft.md"));
        QCOMPARE(backend.resolvePath(QStringLiteral("/tmp/draft.md")).toLocalFile(),
                 QStringLiteral("/tmp/draft.md"));
        QVERIFY(backend.resolvePath(QString()).isEmpty());

        // Once a file is open, a relative name is a sibling of it.
        backend.saveAs(QUrl::fromLocalFile(directory.filePath(QStringLiteral("open.md"))));
        QCOMPARE(backend.resolvePath(QStringLiteral("draft.md")).toLocalFile(),
                 directory.filePath(QStringLiteral("draft.md")));
    }

    void routesKeyPressesThroughVimMode() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        backend.setVimMode(true);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QCOMPARE(window->property("vimStatus").toString(), QStringLiteral("NORMAL"));
        QVERIFY(editor->property("vimNormalMode").toBool());

        editor->setProperty("text", QStringLiteral("alpha beta"));
        editor->setProperty("cursorPosition", 0);

        // Normal mode keeps its keys out of the document and runs commands.
        QTest::keyClick(window, Qt::Key_D);
        QTest::keyClick(window, Qt::Key_W);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("beta"));

        // Insert mode types, and leaves every editor behaviour in place.
        QTest::keyClick(window, Qt::Key_I);
        QCOMPARE(window->property("vimStatus").toString(), QStringLiteral("INSERT"));
        QVERIFY(!editor->property("vimNormalMode").toBool());
        QTest::keyClick(window, Qt::Key_X);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("xbeta"));

        QTest::keyClick(window, Qt::Key_Escape);
        QCOMPARE(window->property("vimStatus").toString(), QStringLiteral("NORMAL"));

        backend.setVimMode(false);
        QCOMPARE(window->property("vimStatus").toString(), QString());
        // Escape stepped the caret back onto the x, so plain typing lands there.
        QTest::keyClick(window, Qt::Key_D);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("dxbeta"));
    }

    void walksDisplayLinesWithJAndK() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        backend.setVimMode(true);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->resize(420, 400);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        // One paragraph per line, long enough that the first one wraps.
        const QString first = QStringLiteral(
            "The paragraph runs on well past the width of the window so that it "
            "has to wrap onto a second display line before it ends.");
        editor->setProperty("text", first + QStringLiteral("\nSecond paragraph."));
        editor->setProperty("cursorPosition", 0);
        QVERIFY(editor->property("width").toReal() > 0);

        // j follows the wrapped text, so it stays inside the first paragraph
        // rather than jumping over the whole of it.
        QTest::keyClick(window, Qt::Key_J);
        const int afterJ = editor->property("cursorPosition").toInt();
        QVERIFY2(afterJ > 0 && afterJ < first.length(),
                 qPrintable(QStringLiteral("j landed at %1, outside the first paragraph")
                                .arg(afterJ)));

        // k comes back to where it started, holding the goal column.
        QTest::keyClick(window, Qt::Key_K);
        QCOMPARE(editor->property("cursorPosition").toInt(), 0);

        // gj is the logical line, so it reaches the second paragraph in one.
        editor->setProperty("cursorPosition", 0);
        QTest::keyClick(window, Qt::Key_G);
        QTest::keyClick(window, Qt::Key_J);
        QCOMPARE(editor->property("cursorPosition").toInt(), first.length() + 1);

        // An operator over j stays logical: dj takes both whole paragraphs,
        // never half of the wrapped one.
        editor->setProperty("cursorPosition", 0);
        QTest::keyClick(window, Qt::Key_D);
        QTest::keyClick(window, Qt::Key_J);
        QCOMPARE(editor->property("text").toString(), QString());
    }

    void opensTheCommandLineFromTheEditor() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        backend.setVimMode(true);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *commandField = window->findChild<QObject *>(QStringLiteral("commandField"));
        QVERIFY(editor);
        QVERIFY(commandField);
        QVERIFY(!window->property("commandOpen").toBool());

        editor->setProperty("text", QStringLiteral("one fish\ntwo fish\nred fish"));
        editor->setProperty("cursorPosition", 0);

        // : opens the command line and takes the keyboard with it.
        QTest::keyClick(window, Qt::Key_Colon);
        QVERIFY(window->property("commandOpen").toBool());
        QVERIFY(commandField->property("activeFocus").toBool());
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("one fish\ntwo fish\nred fish"));

        // A line number jumps and closes the command line.
        typeInto(window, QStringLiteral("2"));
        QTest::keyClick(window, Qt::Key_Return);
        QVERIFY(!window->property("commandOpen").toBool());
        QCOMPARE(editor->property("cursorPosition").toInt(), 9);
        QVERIFY(editor->property("activeFocus").toBool());

        // A substitute runs against the document.
        QTest::keyClick(window, Qt::Key_Colon);
        typeInto(window, QStringLiteral("%s/fish/cat/"));
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("one cat\ntwo cat\nred cat"));
        QCOMPARE(window->property("vimMessage").toString(),
                 QStringLiteral("3 substitutions on 3 lines"));

        // u takes the whole substitute back in one step.
        QTest::keyClick(window, Qt::Key_U);
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("one fish\ntwo fish\nred fish"));
        QCOMPARE(window->property("vimMessage").toString(), QString());

        // Escape abandons a command line without running it.
        QTest::keyClick(window, Qt::Key_Colon);
        typeInto(window, QStringLiteral("%s/fish/dog/"));
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!window->property("commandOpen").toBool());
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("one fish\ntwo fish\nred fish"));

        // With vim mode off, : is just a character again.
        backend.setVimMode(false);
        QTest::keyClick(window, Qt::Key_Colon);
        QVERIFY(!window->property("commandOpen").toBool());
        QVERIFY(editor->property("text").toString().contains(QStringLiteral(":")));
    }

    void startsAtTheTopOfAnOpenedDocument() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("opened.md"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("# Title\n\nA paragraph of prose.\n\nAnother one.\n");
        file.close();

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        backend.setVimMode(true);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        // Opening a document leaves the caret on its first character, where
        // vim starts, rather than adrift on the trailing empty line.
        backend.open(QUrl::fromLocalFile(path));
        QCOMPARE(editor->property("text").toString().left(7), QStringLiteral("# Title"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 0);
        QCOMPARE(window->property("vimStatus").toString(), QStringLiteral("NORMAL"));

        // Reloading from disk does the same.
        editor->setProperty("cursorPosition", 12);
        backend.reloadFromDisk();
        QCOMPARE(editor->property("cursorPosition").toInt(), 0);

        backend.setVimMode(false);
    }

    void remembersVimModePreference() {
        Backend backend;
        backend.setVimMode(false);

        QSignalSpy vimModeSpy(&backend, &Backend::vimModeChanged);
        backend.setVimMode(true);
        QCOMPARE(vimModeSpy.count(), 1);

        Backend restored;
        QVERIFY(restored.vimMode());
        restored.setVimMode(false);

        Backend cleared;
        QVERIFY(!cleared.vimMode());
    }

    void savesAndOpensFromFooterButtons() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QVERIFY(window->findChild<QObject *>(QStringLiteral("sourceEditor")));
        QVERIFY(!window->findChild<QObject *>(QStringLiteral("renderedPreview")));
        QVERIFY(!window->findChild<QObject *>(QStringLiteral("modeToggle")));

        QObject *saveButton = window->findChild<QObject *>(QStringLiteral("saveButton"));
        QObject *openButton = window->findChild<QObject *>(QStringLiteral("openButton"));
        QVERIFY(saveButton);
        QVERIFY(openButton);
        QVERIFY(window->findChild<QObject *>(QStringLiteral("vimButton")));
        QVERIFY(window->findChild<QObject *>(QStringLiteral("vimStatus")));

        QSignalSpy saveDialogSpy(&backend, &Backend::saveDialogRequested);
        QVERIFY(QMetaObject::invokeMethod(saveButton, "clicked"));
        QCOMPARE(saveDialogSpy.count(), 1);

        QSignalSpy openDialogSpy(&backend, &Backend::openDialogRequested);
        QVERIFY(QMetaObject::invokeMethod(openButton, "clicked"));
        QCOMPARE(openDialogSpy.count(), 1);
    }

    void scalesTextWithDesktopTextSize() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 20);

        // `omarchy display text size 16` sets the GNOME factor to 16/12.
        backend.setTextScale(16.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 27);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 27);

        backend.setTextScale(9.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 15);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 15);
    }

    void persistsVimMode() {
        {
            Backend backend;
            QVERIFY(!backend.vimMode());
            backend.setVimMode(true);
        }
        Backend nextWindow;
        const bool remembered = nextWindow.vimMode();
        nextWindow.setVimMode(false);
        QVERIFY(remembered);
    }

    // What "+y and "+p sit on: the clipboard, and the primary selection where
    // the desktop has one.
    void carriesTextThroughTheClipboard() {
        Backend backend;
        backend.setClipboardText(QStringLiteral("yanked\n"));
        QCOMPARE(backend.clipboardText(), QStringLiteral("yanked\n"));

        if (!QGuiApplication::clipboard()->supportsSelection())
            QSKIP("this platform has no primary selection");

        backend.setClipboardText(QStringLiteral("selected"), true);
        QCOMPARE(backend.clipboardText(true), QStringLiteral("selected"));
        QCOMPARE(backend.clipboardText(), QStringLiteral("yanked\n"));
    }

    void vimEditsText() {
        const QString enginePath = QFINDTESTDATA("../src/VimEngine.js");
        QVERIFY(!enginePath.isEmpty());

        QQmlEngine engine;
        QQmlComponent component(&engine);
        const QByteArray harness = R"QML(
            import QtQuick
            import "VimEngine.js" as Vim

            TextEdit {
                id: edit
                textFormat: TextEdit.PlainText

                property string clip: ""
                property var winStub: ({
                    vimYank: function(text) { edit.clip = text; },
                    vimClipboardText: function() { return edit.clip; },
                    vimReplace: function(start, end, replacement) {
                        if (end > start)
                            edit.remove(start, end);
                        if (replacement.length > 0)
                            edit.insert(start, replacement);
                    },
                    openVimCommand: function() {},
                    openVimSearch: function() {},
                    moveSearch: function() {}
                })

                QtObject {
                    id: vimState
                    objectName: "vimState"
                    property string mode: "normal"
                    property string pending: ""
                    property string countText: ""
                    property string opCountText: ""
                    property string op: ""
                    property string prefix: ""
                    property string awaitingChar: ""
                    property string lastFindCmd: ""
                    property string lastFindChar: ""
                    property int anchor: 0
                    property int visualPos: 0
                    property int insertStart: 0
                    property real goalX: -1
                    property bool replaying: false
                    property var keyLog: []
                    property var pendingChangeKeys: null
                    property var lastChange: null
                }

                function load(content, position) {
                    edit.text = content;
                    edit.cursorPosition = position;
                    edit.clip = "";
                    vimState.mode = "normal";
                    vimState.countText = "";
                    vimState.opCountText = "";
                    vimState.op = "";
                    vimState.prefix = "";
                    vimState.awaitingChar = "";
                    vimState.goalX = -1;
                    vimState.lastChange = null;
                }

                function feed(keys) {
                    for (var i = 0; i < keys.length; i++)
                        Vim.handleKey(vimState, edit, winStub,
                                      { key: 0, text: keys[i], modifiers: 0 });
                }

                function esc() {
                    Vim.handleKey(vimState, edit, winStub,
                                  { key: Qt.Key_Escape, text: "", modifiers: 0 });
                }

                function type(content) {
                    var at = edit.cursorPosition;
                    edit.insert(at, content);
                    edit.cursorPosition = at + content.length;
                }
            }
        )QML";
        const QUrl harnessUrl = QUrl::fromLocalFile(
            QFileInfo(enginePath).absolutePath() + QStringLiteral("/VimHarness.qml"));
        component.setData(harness, harnessUrl);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));
        QObject *state = editor->findChild<QObject *>(QStringLiteral("vimState"));
        QVERIFY(state);

        auto load = [&](const QString &content, int position) {
            QMetaObject::invokeMethod(editor.data(), "load",
                                      Q_ARG(QVariant, content), Q_ARG(QVariant, position));
        };
        auto feed = [&](const QString &keys) {
            QMetaObject::invokeMethod(editor.data(), "feed", Q_ARG(QVariant, keys));
        };
        auto esc = [&] { QMetaObject::invokeMethod(editor.data(), "esc"); };
        auto type = [&](const QString &content) {
            QMetaObject::invokeMethod(editor.data(), "type", Q_ARG(QVariant, content));
        };
        auto text = [&] { return editor->property("text").toString(); };
        auto cursor = [&] { return editor->property("cursorPosition").toInt(); };
        auto mode = [&] { return state->property("mode").toString(); };

        // h and l stay on the line; the cursor never crosses a newline.
        load(QStringLiteral("abc\ndef"), 1);
        feed(QStringLiteral("l"));
        QCOMPARE(cursor(), 2);
        feed(QStringLiteral("l"));
        QCOMPARE(cursor(), 2);
        feed(QStringLiteral("h"));
        QCOMPARE(cursor(), 1);
        feed(QStringLiteral("h"));
        QCOMPARE(cursor(), 0);
        feed(QStringLiteral("h"));
        QCOMPARE(cursor(), 0);
        feed(QStringLiteral("2l"));
        QCOMPARE(cursor(), 2);

        // j and k move between lines and remember the goal column.
        load(QStringLiteral("abcdef\nab\nabcdef"), 4);
        feed(QStringLiteral("j"));
        QCOMPARE(cursor(), 8);
        feed(QStringLiteral("j"));
        QCOMPARE(cursor(), 14);
        feed(QStringLiteral("k"));
        QCOMPARE(cursor(), 8);
        feed(QStringLiteral("k"));
        QCOMPARE(cursor(), 4);
        feed(QStringLiteral("j"));
        QCOMPARE(cursor(), 8);
        feed(QStringLiteral("h"));
        QCOMPARE(cursor(), 7);
        feed(QStringLiteral("j"));
        QCOMPARE(cursor(), 10);
        feed(QStringLiteral("2k"));
        QCOMPARE(cursor(), 0);
        feed(QStringLiteral("k"));
        QCOMPARE(cursor(), 0);
        feed(QStringLiteral("2j"));
        QCOMPARE(cursor(), 10);
        feed(QStringLiteral("j"));
        QCOMPARE(cursor(), 10);

        // Word, line, and file motions.
        load(QStringLiteral("alpha beta gamma"), 0);
        feed(QStringLiteral("w"));
        QCOMPARE(cursor(), 6);
        feed(QStringLiteral("e"));
        QCOMPARE(cursor(), 9);
        feed(QStringLiteral("$"));
        QCOMPARE(cursor(), 15);
        feed(QStringLiteral("0"));
        QCOMPARE(cursor(), 0);
        feed(QStringLiteral("2w"));
        QCOMPARE(cursor(), 11);

        load(QStringLiteral("one\ntwo\nthree"), 5);
        feed(QStringLiteral("gg"));
        QCOMPARE(cursor(), 0);
        feed(QStringLiteral("G"));
        QCOMPARE(cursor(), 8);

        // Operators: dw is exclusive, dd linewise, x takes counts.
        load(QStringLiteral("alpha beta"), 0);
        feed(QStringLiteral("dw"));
        QCOMPARE(text(), QStringLiteral("beta"));

        load(QStringLiteral("one\ntwo\nthree"), 0);
        feed(QStringLiteral("dd"));
        QCOMPARE(text(), QStringLiteral("two\nthree"));
        feed(QStringLiteral("p"));
        QCOMPARE(text(), QStringLiteral("two\none\nthree"));

        load(QStringLiteral("abcdef"), 0);
        feed(QStringLiteral("3x"));
        QCOMPARE(text(), QStringLiteral("def"));

        load(QStringLiteral("one\ntwo"), 4);
        feed(QStringLiteral("yyP"));
        QCOMPARE(text(), QStringLiteral("one\ntwo\ntwo"));

        // cw behaves like ce; the insert session repeats with `.`.
        load(QStringLiteral("alpha beta"), 0);
        feed(QStringLiteral("cw"));
        QCOMPARE(mode(), QStringLiteral("insert"));
        QCOMPARE(text(), QStringLiteral(" beta"));
        type(QStringLiteral("delta"));
        esc();
        QCOMPARE(text(), QStringLiteral("delta beta"));
        QCOMPARE(mode(), QStringLiteral("normal"));
        feed(QStringLiteral("w."));
        QCOMPARE(text(), QStringLiteral("delta delta"));

        // Dot repeats operator changes too.
        load(QStringLiteral("aa bb cc"), 0);
        feed(QStringLiteral("dw"));
        feed(QStringLiteral("."));
        QCOMPARE(text(), QStringLiteral("cc"));

        // Character finds.
        load(QStringLiteral("find the x here"), 0);
        feed(QStringLiteral("fx"));
        QCOMPARE(cursor(), 9);
        feed(QStringLiteral("0dtx"));
        QCOMPARE(text(), QStringLiteral("x here"));

        // Join, case toggle, and replace.
        load(QStringLiteral("one\ntwo"), 0);
        feed(QStringLiteral("J"));
        QCOMPARE(text(), QStringLiteral("one two"));

        load(QStringLiteral("abc"), 0);
        feed(QStringLiteral("~"));
        QCOMPARE(text(), QStringLiteral("Abc"));

        load(QStringLiteral("abc"), 0);
        feed(QStringLiteral("rz"));
        QCOMPARE(text(), QStringLiteral("zbc"));

        // Visual mode yanks through the register.
        load(QStringLiteral("alpha beta"), 0);
        feed(QStringLiteral("vey"));
        QCOMPARE(editor->property("clip").toString(), QStringLiteral("alpha"));
        QCOMPARE(mode(), QStringLiteral("normal"));

        // Text objects.
        load(QStringLiteral("say \"hello there\" now"), 6);
        feed(QStringLiteral("di\""));
        QCOMPARE(text(), QStringLiteral("say \"\" now"));

        load(QStringLiteral("para one\nstill one\n\npara two"), 0);
        feed(QStringLiteral("dip"));
        QCOMPARE(text(), QStringLiteral("\npara two"));

        // Insert entries: o opens below, A appends at line end.
        load(QStringLiteral("one\ntwo"), 0);
        feed(QStringLiteral("o"));
        QCOMPARE(mode(), QStringLiteral("insert"));
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));
        QCOMPARE(cursor(), 4);
        esc();
        feed(QStringLiteral("ggA"));
        QCOMPARE(cursor(), 3);
        esc();

        // Normal mode never types plain keys into the document.
        load(QStringLiteral("quiet"), 0);
        feed(QStringLiteral("zQ%"));
        QCOMPARE(text(), QStringLiteral("quiet"));

        // dw on a line's last word stops at the line end.
        load(QStringLiteral("ab\ncd"), 0);
        feed(QStringLiteral("dw"));
        QCOMPARE(text(), QStringLiteral("\ncd"));

        // Deleting the last line takes its newline along, so a trailing
        // empty line can be deleted too.
        load(QStringLiteral("one\ntwo"), 4);
        feed(QStringLiteral("dd"));
        QCOMPARE(text(), QStringLiteral("one"));
        load(QStringLiteral("one\n"), 4);
        feed(QStringLiteral("dd"));
        QCOMPARE(text(), QStringLiteral("one"));

        // Line operators fail at the buffer edges instead of eating a line.
        load(QStringLiteral("one\ntwo"), 4);
        feed(QStringLiteral("dj"));
        QCOMPARE(text(), QStringLiteral("one\ntwo"));
        feed(QStringLiteral("gg"));
        feed(QStringLiteral("dk"));
        QCOMPARE(text(), QStringLiteral("one\ntwo"));

        // A failed change motion keeps the register and stays in normal mode.
        editor->setProperty("clip", QStringLiteral("keep"));
        editor->setProperty("text", QStringLiteral("ab"));
        editor->setProperty("cursorPosition", 0);
        feed(QStringLiteral("ch"));
        QCOMPARE(text(), QStringLiteral("ab"));
        QCOMPARE(mode(), QStringLiteral("normal"));
        QCOMPARE(editor->property("clip").toString(), QStringLiteral("keep"));

        // ge takes the cursor character, being an inclusive motion.
        load(QStringLiteral("alpha beta"), 6);
        feed(QStringLiteral("dge"));
        QCOMPARE(text(), QStringLiteral("alpheta"));

        // A fresh t does not skip an adjacent target; only ; repeats do.
        load(QStringLiteral("axbx"), 0);
        feed(QStringLiteral("tx"));
        QCOMPARE(cursor(), 0);

        // Text objects work from visual mode.
        load(QStringLiteral("hello world"), 1);
        feed(QStringLiteral("viwd"));
        QCOMPARE(text(), QStringLiteral(" world"));

        // Astral characters move and delete whole, never split.
        load(QStringLiteral("a\U0001F600b"), 1);
        feed(QStringLiteral("x"));
        QCOMPARE(text(), QStringLiteral("ab"));
        load(QStringLiteral("a\U0001F600b"), 0);
        feed(QStringLiteral("ll"));
        QCOMPARE(cursor(), 3);

        // Oversized counts finish immediately at the buffer edge.
        load(QStringLiteral("aa bb"), 0);
        feed(QStringLiteral("999999999w"));
        QCOMPARE(cursor(), 4);

        // Joining after trailing whitespace adds no second space.
        load(QStringLiteral("one \ntwo"), 0);
        feed(QStringLiteral("J"));
        QCOMPARE(text(), QStringLiteral("one two"));

        // r with Enter breaks the line.
        load(QStringLiteral("abc"), 1);
        feed(QStringLiteral("r\r"));
        QCOMPARE(text(), QStringLiteral("a\nc"));
        QCOMPARE(cursor(), 2);
    }

    void groupsVimReplaceIntoOneUndo() {
        Backend backend;
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(
                              "import QtQuick\nTextEdit { textFormat: TextEdit.PlainText }"),
                          QUrl());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY(editor);
        backend.attachDocument(editor->property("textDocument").value<QObject *>());

        editor->setProperty("text", QStringLiteral("abc"));
        backend.replaceRange(0, 1, QStringLiteral("z"));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("zbc"));

        QMetaObject::invokeMethod(editor.data(), "undo");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("abc"));
    }

    void remembersLastSaveDirectory() {
        QTemporaryDir saveDirectory;
        QVERIFY(saveDirectory.isValid());

        const QString savedPath = saveDirectory.filePath(QStringLiteral("first.md"));
        Backend savedDocument;
        savedDocument.saveAs(QUrl::fromLocalFile(savedPath));

        Backend nextDocument;
        QSignalSpy saveDialogSpy(&nextDocument, &Backend::saveDialogRequested);
        nextDocument.saveAsDialog();
        QCOMPARE(saveDialogSpy.count(), 1);

        const QUrl suggestedUrl = saveDialogSpy.takeFirst().constFirst().toUrl();
        QCOMPARE(QFileInfo(suggestedUrl.toLocalFile()).absolutePath(),
                 saveDirectory.path());
        QCOMPARE(QFileInfo(suggestedUrl.toLocalFile()).fileName(),
                 QStringLiteral("Untitled.md"));

        QSettings().setValue(QStringLiteral("file/lastSaveDirectory"),
                             saveDirectory.filePath(QStringLiteral("missing")));
        Backend fallbackDocument;
        QSignalSpy fallbackDialogSpy(&fallbackDocument, &Backend::saveDialogRequested);
        fallbackDocument.saveAsDialog();
        const QUrl fallbackUrl = fallbackDialogSpy.takeFirst().constFirst().toUrl();
        QCOMPARE(QFileInfo(fallbackUrl.toLocalFile()).absolutePath(), QDir::homePath());
    }

private:
    struct VimResult {
        QString text;
        int cursor;
        QString mode;
        QString message;
        bool ok;
    };

    // A bare TextEdit driven by the vim engine: keys it does not consume are
    // typed into the document, the way insert mode leaves them to the editor.
    QObject *createVimHarness() {
        const QString vimPath = QFINDTESTDATA("../src/Vim.js");
        if (vimPath.isEmpty())
            return nullptr;

        const QByteArray harness = R"QML(
            import QtQuick
            import "Vim.js" as Vim

            TextEdit {
                id: harness
                property var state: Vim.createState()
                property var host: null
                property var unhandled: []
                property var calls: []
                property string message: ""
                property bool ok: true
                property string clipboardText: ""
                property string selectionText: ""

                Component.onCompleted: host = Vim.createHost(this, {
                    clipboard: function(fromSelection) {
                        return fromSelection ? harness.selectionText : harness.clipboardText;
                    },
                    setClipboard: function(text, toSelection) {
                        if (toSelection)
                            harness.selectionText = text;
                        else
                            harness.clipboardText = text;
                    },
                    save: function() { record("save"); },
                    saveAs: function(path) { record("saveAs:" + path); },
                    saveAndQuit: function() { record("saveAndQuit"); },
                    quit: function(force) { record("quit:" + force); },
                    open: function(path, force) { record("open:" + path + ":" + force); },
                    clearSearch: function() { record("clearSearch"); },
                    commandLine: function(prefill) { record("commandLine:" + prefill); }
                })

                function record(call) { calls = calls.concat([call]); }

                function reset(startText, startCursor) {
                    state = Vim.createState();
                    unhandled = [];
                    calls = [];
                    message = "";
                    ok = true;
                    text = startText;
                    cursorPosition = startCursor;
                }

                function ex(command) {
                    var result = Vim.runCommand(state, host, command);
                    message = result.message;
                    ok = result.ok;
                }

                function feed(sequence) {
                    var keys = tokenize(sequence);
                    for (var i = 0; i < keys.length; i++) {
                        if (Vim.handleKey(state, host, keys[i]))
                            continue;
                        unhandled = unhandled.concat([keys[i]]);
                        if (keys[i] === "Return")
                            insert(cursorPosition, "\n");
                        else if (keys[i] === "Backspace")
                            remove(Math.max(0, cursorPosition - 1), cursorPosition);
                        else if (keys[i].length === 1)
                            insert(cursorPosition, keys[i]);
                    }
                }

                function mode() { return state.mode; }

                function status() { return Vim.statusText(state); }

                // "<Esc>", "<CR>" and "<C-r>" name the keys that are not
                // a single character.
                function tokenize(sequence) {
                    var keys = [];
                    for (var i = 0; i < sequence.length; i++) {
                        if (sequence.charAt(i) !== "<") {
                            keys.push(sequence.charAt(i));
                            continue;
                        }
                        var close = sequence.indexOf(">", i);
                        var name = sequence.slice(i + 1, close);
                        keys.push(name === "Esc" ? "Escape" : name === "CR" ? "Return" : name);
                        i = close;
                    }
                    return keys;
                }
            }
        )QML";

        QQmlComponent component(&m_vimEngine);
        component.setData(harness, QUrl::fromLocalFile(
            QFileInfo(vimPath).absolutePath() + QStringLiteral("/VimHarness.qml")));
        if (!component.isReady()) {
            qWarning("%s", qPrintable(component.errorString()));
            return nullptr;
        }
        return component.create();
    }

    // QTest::keyClicks only takes widgets, so type into a QWindow by hand.
    void typeInto(QQuickWindow *window, const QString &characters) {
        for (const QChar &character : characters)
            QTest::keyClick(window, character.toLatin1());
    }

    VimResult readVim(QObject *editor) {
        QVariant mode;
        QMetaObject::invokeMethod(editor, "mode", Q_RETURN_ARG(QVariant, mode));
        return {editor->property("text").toString(),
                editor->property("cursorPosition").toInt(),
                mode.toString(),
                editor->property("message").toString(),
                editor->property("ok").toBool()};
    }

    VimResult runVim(QObject *editor, const QString &text, int cursor, const QString &keys) {
        QMetaObject::invokeMethod(editor, "reset", Q_ARG(QVariant, text), Q_ARG(QVariant, cursor));
        QMetaObject::invokeMethod(editor, "feed", Q_ARG(QVariant, keys));
        return readVim(editor);
    }

    VimResult runEx(QObject *editor, const QString &text, int cursor, const QString &command) {
        QMetaObject::invokeMethod(editor, "reset", Q_ARG(QVariant, text), Q_ARG(QVariant, cursor));
        QMetaObject::invokeMethod(editor, "ex", Q_ARG(QVariant, command));
        return readVim(editor);
    }

    QTemporaryDir m_settingsDirectory;
    QQmlEngine m_vimEngine;
};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"
