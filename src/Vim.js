.pragma library
.import "EditorMutations.js" as EditorMutations

// A small vim engine for the editor. Main.qml turns a key event into a name
// (see vimKeyName there) and hands it to handleKey(), which reports whether it
// consumed the key. Insert mode consumes nothing but Escape, so smart returns,
// Markdown paste and every other editor behaviour keep working while typing.

// ---------------------------------------------------------------- state

function createState() {
    return {
        mode: "normal",     // normal | insert | visual | vline
        count: "",          // digits typed before an operator
        operatorCount: "",  // digits typed between an operator and its motion
        operator: "",       // d, c or y awaiting a motion
        pending: "",        // key awaiting an argument: g, r, f, F, t, T
        anchor: -1,         // fixed end of the visual selection
        head: -1,           // moving end of the visual selection
        column: -1,         // column j/k aim for across short lines
        register: {text: "", linewise: false},
        lastFind: null,     // {command, character} for ; and ,
        lastChange: null,   // {keys, insert} replayed by .
        insertSession: null,
        keys: [],
        replaying: false
    };
}

function statusText(state) {
    var label = state.mode === "insert" ? "INSERT"
        : state.mode === "visual" ? "VISUAL"
        : state.mode === "vline" ? "V-LINE"
        : "NORMAL";
    var pending = state.count + state.operator + state.operatorCount + state.pending;
    return pending.length > 0 ? label + " " + pending : label;
}

// The engine only ever touches the editor through this wrapper, so the tests
// can drive a bare TextEdit while Main.qml adds the application hooks.
function createHost(editor, hooks) {
    hooks = hooks || {};
    return {
        text: function() { return editor.text; },
        cursor: function() { return editor.cursorPosition; },
        setCursor: function(position) {
            editor.deselect();
            editor.cursorPosition = clamp(editor.text, position);
        },
        select: function(from, to) {
            editor.cursorPosition = clamp(editor.text, from);
            editor.moveCursorSelection(clamp(editor.text, to));
        },
        deselect: function() { editor.deselect(); },
        replace: function(start, end, replacement) {
            EditorMutations.replaceRange(editor, start, end, replacement);
        },
        undo: function() { editor.undo(); },
        redo: function() { editor.redo(); },
        // A screen line up or down, for gj and gk under wrapped paragraphs.
        displayLine: function(position, delta) {
            var rect = editor.cursorRectangle;
            if (rect.height <= 0)
                return position;
            return editor.positionAt(rect.x, rect.y + rect.height * (delta + 0.5));
        },
        // Grouping every command into one undo step keeps u the inverse of
        // the command that ran, rather than of the edits it happened to make.
        beginChange: function() { if (hooks.beginChange) hooks.beginChange(); },
        endChange: function() { if (hooks.endChange) hooks.endChange(); },
        // Where a motion may leave the caret: markdown markers are drawn at
        // no width, so resting on one would look like the caret stopped
        // moving. The application hook skips them the way the arrow keys do.
        settle: function(position, direction) {
            return hooks.settle ? hooks.settle(position, direction) : position;
        },
        page: function(direction) { if (hooks.page) hooks.page(direction); },
        search: function() { if (hooks.search) hooks.search(); },
        searchNext: function(direction) { if (hooks.searchNext) hooks.searchNext(direction); }
    };
}

// ---------------------------------------------------------------- text

function clamp(text, position) {
    return Math.max(0, Math.min(text.length, position));
}

function lineStart(text, position) {
    if (position <= 0)
        return 0;
    return text.lastIndexOf("\n", position - 1) + 1;
}

function lineEnd(text, position) {
    var index = text.indexOf("\n", clamp(text, position));
    return index < 0 ? text.length : index;
}

function firstNonBlank(text, position) {
    var start = lineStart(text, position);
    var end = lineEnd(text, position);
    var i = start;
    while (i < end && (text.charAt(i) === " " || text.charAt(i) === "\t"))
        i++;
    return i < end ? i : start;
}

function indentOf(text, position) {
    var start = lineStart(text, position);
    return text.slice(start, firstNonBlank(text, position));
}

function lineNumberPosition(text, number) {
    var position = 0;
    for (var line = 1; line < number; line++) {
        var end = lineEnd(text, position);
        if (end >= text.length)
            return position;
        position = end + 1;
    }
    return position;
}

// Vim keeps the caret on a character, never on the line break past it.
function clampNormal(text, position) {
    position = clamp(text, position);
    var start = lineStart(text, position);
    var end = lineEnd(text, position);
    return position > start && position >= end ? end - 1 : position;
}

// 0 blank, 1 punctuation, 2 word. W and B treat every non-blank alike.
function charClass(character, big) {
    if (character === "" || /\s/.test(character))
        return 0;
    if (big)
        return 2;
    return /[A-Za-z0-9_\u00c0-\uffff]/.test(character) ? 2 : 1;
}

function wordForward(text, position, big) {
    var i = clamp(text, position);
    var start = charClass(text.charAt(i), big);
    while (i < text.length && charClass(text.charAt(i), big) === start && start !== 0)
        i++;
    while (i < text.length && charClass(text.charAt(i), big) === 0)
        i++;
    return i;
}

function wordBackward(text, position, big) {
    var i = clamp(text, position) - 1;
    while (i >= 0 && charClass(text.charAt(i), big) === 0)
        i--;
    if (i < 0)
        return 0;
    var start = charClass(text.charAt(i), big);
    while (i >= 0 && charClass(text.charAt(i), big) === start)
        i--;
    return i + 1;
}

function wordEnd(text, position, big) {
    var i = clamp(text, position) + 1;
    while (i < text.length && charClass(text.charAt(i), big) === 0)
        i++;
    if (i >= text.length)
        return Math.max(0, text.length - 1);
    var start = charClass(text.charAt(i), big);
    while (i + 1 < text.length && charClass(text.charAt(i + 1), big) === start)
        i++;
    return i;
}

// Unlike e, cw on the last character of a word stops there instead of
// running on to the end of the next one.
function changeWordEnd(text, position, big) {
    var here = charClass(text.charAt(position), big);
    if (here !== 0 && charClass(text.charAt(position + 1), big) !== here)
        return position;
    return wordEnd(text, position, big);
}

function paragraphForward(text, position) {
    var i = lineEnd(text, position);
    while (i < text.length) {
        i = i + 1;
        if (lineEnd(text, i) === i)
            return i;
        i = lineEnd(text, i);
    }
    return text.length;
}

function paragraphBackward(text, position) {
    var i = lineStart(text, position);
    while (i > 0) {
        i = lineStart(text, i - 1);
        if (lineEnd(text, i) === i)
            return i;
    }
    return 0;
}

function findInLine(text, position, command, character, count) {
    var forward = command === "f" || command === "t";
    var start = lineStart(text, position);
    var end = lineEnd(text, position);
    var i = position;
    for (var found = 0; found < count; found++) {
        if (forward) {
            i = text.indexOf(character, i + 1);
            if (i < 0 || i >= end)
                return -1;
        } else {
            i = text.lastIndexOf(character, i - 1);
            if (i < 0 || i < start)
                return -1;
        }
    }
    if (command === "t")
        return i - 1;
    if (command === "T")
        return i + 1;
    return i;
}

function repeatString(value, count) {
    var result = "";
    for (var i = 0; i < count; i++)
        result += value;
    return result;
}

// ---------------------------------------------------------------- caret

function caret(state, host) {
    return state.mode === "visual" || state.mode === "vline"
        ? state.head
        : host.cursor();
}

function moveCaret(state, host, position) {
    var text = host.text();
    if (state.mode === "visual" || state.mode === "vline") {
        state.head = clampNormal(text, position);
        showSelection(state, host);
        return;
    }
    host.setCursor(clampNormal(text, position));
}

function showSelection(state, host) {
    var text = host.text();
    var head = state.head;
    var anchor = state.anchor;
    if (state.mode === "vline") {
        var from = lineStart(text, Math.min(anchor, head));
        var to = Math.min(text.length, lineEnd(text, Math.max(anchor, head)) + 1);
        if (head <= anchor)
            host.select(to, from);
        else
            host.select(from, to);
    } else if (head >= anchor) {
        host.select(anchor, Math.min(text.length, head + 1));
    } else {
        host.select(Math.min(text.length, anchor + 1), head);
    }
}

function selectionRange(state, host) {
    var text = host.text();
    var from = Math.min(state.anchor, state.head);
    var to = Math.max(state.anchor, state.head);
    if (state.mode === "vline")
        return {start: from, end: to, linewise: true};
    return {start: from, end: Math.min(text.length, to + 1), linewise: false};
}

// ---------------------------------------------------------------- modes

function enterInsert(state, host, position) {
    host.setCursor(position);
    state.mode = "insert";
    state.insertSession = {
        keys: state.keys.slice(),
        start: host.cursor(),
        text: host.text()
    };
    resetPending(state);
}

function leaveInsert(state, host) {
    var session = state.insertSession;
    state.insertSession = null;
    state.mode = "normal";
    if (session && !state.replaying)
        state.lastChange = {keys: session.keys,
                            insert: insertDelta(session.text, host.text(), session.start)};
    host.setCursor(clampNormal(host.text(), host.cursor() - 1));
    resetPending(state);
}

// What an insert session did to the document, as "delete this many characters
// behind the caret, then type this". Replaying the result rather than the
// keystrokes keeps `.` honest about list continuation and Markdown paste,
// which rewrite what a key would otherwise have typed.
function insertDelta(before, after, start) {
    var shared = Math.min(before.length, after.length);
    var prefix = 0;
    while (prefix < shared && before.charAt(prefix) === after.charAt(prefix))
        prefix++;
    var suffix = 0;
    while (suffix < shared - prefix
           && before.charAt(before.length - 1 - suffix) === after.charAt(after.length - 1 - suffix))
        suffix++;
    return {back: Math.max(0, Math.min(start, start - prefix)),
            text: after.slice(prefix, after.length - suffix)};
}

function enterVisual(state, host, mode) {
    if (state.mode === mode) {
        leaveVisual(state, host);
        return;
    }
    if (state.mode !== "visual" && state.mode !== "vline") {
        state.anchor = host.cursor();
        state.head = host.cursor();
    }
    state.mode = mode;
    showSelection(state, host);
}

function leaveVisual(state, host) {
    var head = state.head;
    state.mode = "normal";
    state.anchor = -1;
    state.head = -1;
    host.setCursor(clampNormal(host.text(), head));
}

function resetPending(state) {
    state.count = "";
    state.operatorCount = "";
    state.operator = "";
    state.pending = "";
}

function effectiveCount(state) {
    var outer = state.count === "" ? 1 : parseInt(state.count, 10);
    var inner = state.operatorCount === "" ? 1 : parseInt(state.operatorCount, 10);
    return Math.max(1, outer * inner);
}

function commitChange(state) {
    if (!state.replaying && state.mode !== "visual" && state.mode !== "vline")
        state.lastChange = {keys: state.keys.slice(), insert: null};
}

// ---------------------------------------------------------------- keys

function handleKey(state, host, key) {
    if (key === "")
        return false;

    if (state.mode === "insert") {
        if (key === "Escape" || key === "C-[" || key === "C-c") {
            leaveInsert(state, host);
            return true;
        }
        return false;
    }

    if (!state.replaying) {
        if (state.count === "" && state.operator === "" && state.pending === "")
            state.keys = [key];
        else
            state.keys.push(key);
    }

    host.beginChange();
    var handled = dispatch(state, host, key);
    host.endChange();
    return handled;
}

function dispatch(state, host, key) {
    if (state.pending !== "")
        return argument(state, host, key);

    if (key === "Escape" || key === "C-[" || key === "C-c") {
        if (state.count !== "" || state.operator !== "")
            resetPending(state);
        else if (state.mode === "visual" || state.mode === "vline")
            leaveVisual(state, host);
        else
            host.deselect();
        return true;
    }

    // 0 only counts as a digit once one is already typed; on its own it is
    // the motion to the start of the line.
    var digits = state.operator === "" ? state.count : state.operatorCount;
    if (/^[1-9]$/.test(key) || (key === "0" && digits !== "")) {
        if (state.operator === "")
            state.count += key;
        else
            state.operatorCount += key;
        return true;
    }

    if (key === "g" || key === "r" || key === "f" || key === "F" || key === "t" || key === "T") {
        state.pending = key;
        return true;
    }

    if (key === "d" || key === "c" || key === "y")
        return operator(state, host, key);

    var motion = evaluateMotion(state, host, key, effectiveCount(state));
    if (motion)
        return applyMotion(state, host, motion);

    if (state.operator !== "") {
        resetPending(state);
        return true;
    }

    return simpleCommand(state, host, key);
}

function argument(state, host, key) {
    var pending = state.pending;
    state.pending = "";

    if (pending === "g") {
        if (key === "g") {
            var target = state.count === "" && state.operatorCount === ""
                ? 0
                : lineNumberPosition(host.text(), effectiveCount(state));
            return applyMotion(state, host, {position: target, linewise: true, toFirstNonBlank: true});
        }
        if (key === "j" || key === "k") {
            var delta = key === "j" ? effectiveCount(state) : -effectiveCount(state);
            return applyMotion(state, host, {position: host.displayLine(caret(state, host), delta)});
        }
        resetPending(state);
        return true;
    }

    if (pending === "r") {
        if (key.length === 1) {
            if (state.mode === "visual" || state.mode === "vline") {
                var range = selectionRange(state, host);
                leaveVisual(state, host);
                host.setCursor(range.start);
                replaceCharacters(state, host, key, range.end - range.start);
            } else {
                replaceCharacters(state, host, key, effectiveCount(state));
            }
        }
        resetPending(state);
        return true;
    }

    if (key.length !== 1) {
        resetPending(state);
        return true;
    }

    state.lastFind = {command: pending, character: key};
    return findMotion(state, host, pending, key, effectiveCount(state));
}

function findMotion(state, host, command, character, count) {
    var position = findInLine(host.text(), caret(state, host), command, character, count);
    if (position < 0) {
        resetPending(state);
        return true;
    }
    var forward = command === "f" || command === "t";
    return applyMotion(state, host, {position: position, inclusive: forward});
}

function operator(state, host, key) {
    if (state.mode === "visual" || state.mode === "vline") {
        var range = selectionRange(state, host);
        state.mode = "normal";
        state.anchor = -1;
        state.head = -1;
        host.deselect();
        applyOperator(state, host, key, range.start, range.end, range.linewise);
        return true;
    }

    if (state.operator === key) {
        // dd, cc and yy act on whole lines, count of them.
        var text = host.text();
        var start = host.cursor();
        var end = start;
        for (var line = 1; line < effectiveCount(state); line++) {
            var next = lineEnd(text, end);
            if (next >= text.length)
                break;
            end = next + 1;
        }
        applyOperator(state, host, key, start, end, true);
        return true;
    }

    if (state.operator !== "") {
        resetPending(state);
        return true;
    }

    state.operator = key;
    return true;
}

function applyMotion(state, host, motion) {
    var text = host.text();
    var origin = caret(state, host);
    if (state.operator !== "") {
        var from = host.cursor();
        var to = motion.position;
        if (motion.inclusive && to >= from)
            to = Math.min(text.length, to + 1);
        applyOperator(state, host, state.operator,
                      Math.min(from, to), Math.max(from, to), !!motion.linewise);
        return true;
    }

    var position = motion.toFirstNonBlank ? firstNonBlank(text, motion.position) : motion.position;
    moveCaret(state, host, host.settle(position, position >= origin ? 1 : -1));
    if (!motion.keepColumn)
        state.column = -1;
    resetPending(state);
    return true;
}

function evaluateMotion(state, host, key, count) {
    var text = host.text();
    var position = caret(state, host);
    var i;

    switch (key) {
    case "h":
    case "Left":
    case "Backspace":
        return {position: Math.max(lineStart(text, position), position - count)};
    case "l":
    case "Right":
    case " ":
        return {position: Math.min(lineEnd(text, position), position + count)};
    case "j":
    case "Down":
    case "Return":
        return {position: verticalMove(state, text, position, count), keepColumn: true, linewise: true};
    case "k":
    case "Up":
        return {position: verticalMove(state, text, position, -count), keepColumn: true, linewise: true};
    case "w":
    case "W":
        // cw is vim's odd one out: on a non-blank it changes to the end of
        // the word, leaving the space after it alone.
        if (state.operator === "c" && charClass(text.charAt(position), key === "W") !== 0) {
            for (i = 0; i < count; i++)
                position = i === 0
                    ? changeWordEnd(text, position, key === "W")
                    : wordEnd(text, position, key === "W");
            return {position: position, inclusive: true};
        }
        for (i = 0; i < count; i++)
            position = wordForward(text, position, key === "W");
        return {position: position};
    case "b":
    case "B":
        for (i = 0; i < count; i++)
            position = wordBackward(text, position, key === "B");
        return {position: position};
    case "e":
    case "E":
        for (i = 0; i < count; i++)
            position = wordEnd(text, position, key === "E");
        return {position: position, inclusive: true};
    case "0":
    case "Home":
        return {position: lineStart(text, position)};
    case "^":
        return {position: firstNonBlank(text, position)};
    case "$":
    case "End":
        for (i = 1; i < count; i++)
            position = Math.min(text.length, lineEnd(text, position) + 1);
        return {position: lineEnd(text, position)};
    case "G":
        return {position: state.count === "" && state.operatorCount === ""
                    ? lineStart(text, text.length)
                    : lineNumberPosition(text, count),
                linewise: true, toFirstNonBlank: true};
    case "{":
        for (i = 0; i < count; i++)
            position = paragraphBackward(text, position);
        return {position: position};
    case "}":
        for (i = 0; i < count; i++)
            position = paragraphForward(text, position);
        return {position: position};
    case ";":
    case ",":
        if (!state.lastFind)
            return null;
        var command = state.lastFind.command;
        if (key === ",")
            command = {f: "F", F: "f", t: "T", T: "t"}[command];
        var found = findInLine(text, position, command, state.lastFind.character, count);
        if (found < 0)
            return null;
        return {position: found, inclusive: command === "f" || command === "t"};
    }
    return null;
}

// j and k walk logical lines, as in vim, and hold the column they started
// from so passing through a short line does not drag the caret left.
function verticalMove(state, text, position, delta) {
    var column = state.column >= 0 ? state.column : position - lineStart(text, position);
    state.column = column;
    var start = lineStart(text, position);
    for (var i = 0; i < Math.abs(delta); i++) {
        if (delta > 0) {
            var end = lineEnd(text, start);
            if (end >= text.length)
                break;
            start = end + 1;
        } else {
            if (start === 0)
                break;
            start = lineStart(text, start - 1);
        }
    }
    return Math.min(start + column, lineEnd(text, start));
}

// ---------------------------------------------------------------- edits

function applyOperator(state, host, op, start, end, linewise) {
    var text = host.text();
    var cursor = host.cursor();
    start = clamp(text, start);
    end = Math.max(start, clamp(text, end));

    if (linewise) {
        start = lineStart(text, start);
        end = Math.min(text.length, lineEnd(text, end) + 1);
    }

    var slice = text.slice(start, end);
    if (linewise && !/\n$/.test(slice))
        slice += "\n";

    if (op === "y") {
        state.register = {text: slice, linewise: linewise};
        if (start < (linewise ? lineStart(text, cursor) : cursor))
            host.setCursor(linewise ? firstNonBlank(text, start) : start);
        resetPending(state);
        return;
    }

    state.register = {text: slice, linewise: linewise};

    if (op === "c" && linewise) {
        // cc empties the line but keeps it, and keeps its indentation so
        // rewriting a list item does not lose the bullet's nesting.
        var indent = indentOf(text, start);
        var contentEnd = lineEnd(text, end > start ? end - 1 : start);
        host.replace(start, contentEnd, indent);
        enterInsert(state, host, start + indent.length);
        return;
    }

    // Deleting through the end of the document takes the line break before it,
    // so the lines above do not gain a trailing empty line.
    if (linewise && end >= text.length && start > 0)
        start -= 1;

    host.replace(start, end, "");

    if (op === "c") {
        enterInsert(state, host, start);
        return;
    }

    var updated = host.text();
    host.setCursor(linewise
        ? firstNonBlank(updated, clamp(updated, start))
        : clampNormal(updated, start));
    commitChange(state);
    resetPending(state);
}

function paste(state, host, after, count) {
    if (state.register.text === "")
        return;

    var text = host.text();
    var position = host.cursor();
    var payload = repeatString(state.register.text, count);

    if (state.register.linewise) {
        var at = after ? Math.min(text.length, lineEnd(text, position) + 1) : lineStart(text, position);
        var body = payload;
        // A last line without its own break needs one added ahead of the paste.
        if (at === text.length && text.length > 0 && text.charAt(text.length - 1) !== "\n") {
            body = "\n" + payload.replace(/\n$/, "");
            host.replace(at, at, body);
            host.setCursor(firstNonBlank(host.text(), at + 1));
        } else {
            host.replace(at, at, body);
            host.setCursor(firstNonBlank(host.text(), at));
        }
    } else {
        var target = after ? Math.min(lineEnd(text, position), position + 1) : position;
        host.replace(target, target, payload);
        host.setCursor(clampNormal(host.text(), target + payload.length - 1));
    }
    commitChange(state);
}

function deleteCharacters(state, host, forward, count) {
    var text = host.text();
    var position = host.cursor();
    var start = forward ? position : Math.max(lineStart(text, position), position - count);
    var end = forward ? Math.min(lineEnd(text, position), position + count) : position;
    if (start === end)
        return;
    state.register = {text: text.slice(start, end), linewise: false};
    host.replace(start, end, "");
    host.setCursor(clampNormal(host.text(), start));
    commitChange(state);
}

function replaceCharacters(state, host, character, count) {
    var text = host.text();
    var position = host.cursor();
    var end = Math.min(lineEnd(text, position), position + count);
    if (end === position)
        return;
    host.replace(position, end, repeatString(character, end - position));
    host.setCursor(end - 1);
    commitChange(state);
}

function toggleCase(state, host, count) {
    var text = host.text();
    var position = host.cursor();
    var end = Math.min(lineEnd(text, position), position + count);
    if (end === position)
        return;
    var slice = text.slice(position, end).replace(/./g, function(character) {
        var upper = character.toUpperCase();
        return character === upper ? character.toLowerCase() : upper;
    });
    host.replace(position, end, slice);
    host.setCursor(clampNormal(host.text(), end));
    commitChange(state);
}

function joinLines(state, host, count) {
    var text = host.text();
    var position = host.cursor();
    var joined = 0;
    for (var i = 0; i < Math.max(1, count - 1); i++) {
        text = host.text();
        var end = lineEnd(text, position);
        if (end >= text.length)
            break;
        var next = end + 1;
        while (next < text.length && (text.charAt(next) === " " || text.charAt(next) === "\t"))
            next++;
        var separator = end === lineStart(text, position) || next === lineEnd(text, next) ? "" : " ";
        host.replace(end, next, separator);
        host.setCursor(end);
        position = end;
        joined++;
    }
    if (joined > 0)
        commitChange(state);
}

function openLine(state, host, below) {
    var text = host.text();
    var position = host.cursor();
    if (below) {
        var end = lineEnd(text, position);
        host.replace(end, end, "\n");
        enterInsert(state, host, Math.min(host.text().length, end + 1));
    } else {
        var start = lineStart(text, position);
        host.replace(start, start, "\n");
        enterInsert(state, host, start);
    }
}

function repeatChange(state, host) {
    var change = state.lastChange;
    if (!change)
        return;

    state.replaying = true;
    resetPending(state);
    for (var i = 0; i < change.keys.length; i++)
        handleKey(state, host, change.keys[i]);

    if (state.mode === "insert") {
        if (change.insert) {
            var position = host.cursor();
            host.replace(Math.max(0, position - change.insert.back), position, change.insert.text);
        }
        state.mode = "normal";
        state.insertSession = null;
        host.setCursor(clampNormal(host.text(), host.cursor() - 1));
    }
    state.replaying = false;
    resetPending(state);
}

// ---------------------------------------------------------------- commands

function simpleCommand(state, host, key) {
    var text = host.text();
    var position = caret(state, host);
    var count = effectiveCount(state);
    var visual = state.mode === "visual" || state.mode === "vline";

    switch (key) {
    case "i":
        if (visual)
            return true;
        enterInsert(state, host, position);
        return true;
    case "I":
        if (visual)
            return true;
        enterInsert(state, host, firstNonBlank(text, position));
        return true;
    case "a":
        if (visual)
            return true;
        enterInsert(state, host, Math.min(lineEnd(text, position), position + 1));
        return true;
    case "A":
        if (visual)
            return true;
        enterInsert(state, host, lineEnd(text, position));
        return true;
    case "o":
        if (visual) {
            var anchor = state.anchor;
            state.anchor = state.head;
            state.head = anchor;
            showSelection(state, host);
            return true;
        }
        openLine(state, host, true);
        return true;
    case "O":
        if (visual)
            return true;
        openLine(state, host, false);
        return true;
    case "v":
        enterVisual(state, host, "visual");
        return true;
    case "V":
        enterVisual(state, host, "vline");
        return true;
    case "x":
    case "Delete":
        if (visual)
            return operator(state, host, "d");
        deleteCharacters(state, host, true, count);
        resetPending(state);
        return true;
    case "X":
        if (visual)
            return operator(state, host, "d");
        deleteCharacters(state, host, false, count);
        resetPending(state);
        return true;
    case "s":
        if (visual)
            return operator(state, host, "c");
        applyOperator(state, host, "c", position,
                      Math.min(lineEnd(text, position), position + count), false);
        return true;
    case "S":
        if (visual) {
            state.mode = "vline";
            return operator(state, host, "c");
        }
        state.operator = "c";
        return operator(state, host, "c");
    case "D":
        if (visual) {
            state.mode = "vline";
            return operator(state, host, "d");
        }
        applyOperator(state, host, "d", position, lineEnd(text, position), false);
        return true;
    case "C":
        if (visual) {
            state.mode = "vline";
            return operator(state, host, "c");
        }
        applyOperator(state, host, "c", position, lineEnd(text, position), false);
        return true;
    case "Y":
        if (visual) {
            state.mode = "vline";
            return operator(state, host, "y");
        }
        state.operator = "y";
        return operator(state, host, "y");
    case "p":
    case "P":
        if (visual) {
            var range = selectionRange(state, host);
            var register = {text: state.register.text, linewise: state.register.linewise};
            state.mode = "normal";
            state.anchor = -1;
            state.head = -1;
            host.deselect();
            applyOperator(state, host, "d", range.start, range.end, range.linewise);
            state.register = register;
            paste(state, host, false, count);
            resetPending(state);
            return true;
        }
        paste(state, host, key === "p", count);
        resetPending(state);
        return true;
    case "u":
        host.undo();
        host.setCursor(clampNormal(host.text(), host.cursor()));
        resetPending(state);
        return true;
    case "C-r":
        host.redo();
        host.setCursor(clampNormal(host.text(), host.cursor()));
        resetPending(state);
        return true;
    case "J":
        if (visual) {
            var span = Math.max(2, countLines(text, Math.min(state.anchor, state.head),
                                              Math.max(state.anchor, state.head)));
            var top = lineStart(text, Math.min(state.anchor, state.head));
            state.mode = "normal";
            state.anchor = -1;
            state.head = -1;
            host.setCursor(top);
            joinLines(state, host, span);
            resetPending(state);
            return true;
        }
        joinLines(state, host, count);
        resetPending(state);
        return true;
    case "~":
        toggleCase(state, host, count);
        resetPending(state);
        return true;
    case ".":
        resetPending(state);
        repeatChange(state, host);
        return true;
    case "/":
        resetPending(state);
        host.search();
        return true;
    case "n":
        resetPending(state);
        host.searchNext(1);
        return true;
    case "N":
        resetPending(state);
        host.searchNext(-1);
        return true;
    case "C-d":
    case "PageDown":
        resetPending(state);
        host.page(1);
        return true;
    case "C-u":
    case "PageUp":
        resetPending(state);
        host.page(-1);
        return true;
    }

    // Normal mode swallows everything else, so stray letters never reach the
    // document; modified keys fall through to the window's shortcuts.
    resetPending(state);
    return key.indexOf("C-") !== 0;
}

function countLines(text, start, end) {
    var lines = 1;
    var position = lineStart(text, start);
    while (position < lineStart(text, end)) {
        position = lineEnd(text, position) + 1;
        lines++;
    }
    return lines;
}
