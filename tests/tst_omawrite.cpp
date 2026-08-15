#include <QtTest>
#include <QFont>
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

        // j and k hold the column they started from across a short line.
        QCOMPARE(runVim(editor.data(), QStringLiteral("abcdef\nx\nabcdef"), 4,
                        QStringLiteral("jj")).cursor, 13);

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

        // Normal mode never lets a plain letter reach the document.
        const VimResult swallowed = runVim(editor.data(), QStringLiteral("text"), 0,
                                           QStringLiteral("zq"));
        QCOMPARE(swallowed.text, QStringLiteral("text"));

        // Insert mode hands every key back to the editor.
        QCOMPARE(editor->property("unhandled").toStringList(), QStringList());
        runVim(editor.data(), QStringLiteral("x"), 0, QStringLiteral("ia"));
        QCOMPARE(editor->property("unhandled").toStringList(), QStringList{QStringLiteral("a")});
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
                property var state: Vim.createState()
                property var host: null
                property var unhandled: []
                property var calls: []
                property string message: ""
                property bool ok: true

                Component.onCompleted: host = Vim.createHost(this, {
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
