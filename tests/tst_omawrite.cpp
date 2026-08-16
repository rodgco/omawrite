#include <QtTest>
#include <QFont>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>

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
        QVERIFY(window->findChild<QObject *>(QStringLiteral("vimModeLabel")));

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
    QTemporaryDir m_settingsDirectory;
};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"
