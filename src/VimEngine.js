.pragma library

// Modal editing for the writing pane: NORMAL, INSERT, and VISUAL modes with
// the motions and operators a prose document calls for. The library holds no
// state of its own; per-window state lives on the `vim` object QML hands in,
// which keeps the engine shareable across windows and testable in isolation.

// Words include apostrophes so contractions move as one word, a deliberate
// prose deviation from vim's iskeyword.
var wordCharacters = /[A-Za-z0-9_'’À-ɏ]/;

function charClass(character) {
    if (character === undefined || /\s/.test(character))
        return 0;
    return wordCharacters.test(character) ? 1 : 2;
}

// Characters outside the basic plane occupy two UTF-16 units; single-column
// steps have to jump both so emoji never get cut in half.
function stepForward(text, position, count) {
    var i = position;
    while (count-- > 0 && i < text.length) {
        var code = text.charCodeAt(i);
        i += (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.length) ? 2 : 1;
    }
    return i;
}

function stepBackward(text, position, count) {
    var i = position;
    while (count-- > 0 && i > 0) {
        var code = text.charCodeAt(i - 1);
        i -= (code >= 0xDC00 && code <= 0xDFFF && i >= 2) ? 2 : 1;
    }
    return i;
}

function lineStartAt(text, position) {
    if (position <= 0)
        return 0;
    return text.lastIndexOf("\n", position - 1) + 1;
}

function lineEndAt(text, position) {
    var end = text.indexOf("\n", position);
    return end === -1 ? text.length : end;
}

function lineIsEmpty(text, position) {
    var start = lineStartAt(text, position);
    return lineEndAt(text, start) === start;
}

function firstNonBlankAt(text, position) {
    var start = lineStartAt(text, position);
    var end = lineEndAt(text, start);
    while (start < end && (text[start] === " " || text[start] === "\t"))
        start++;
    return start;
}

// The normal-mode cursor sits on a character, never past the last one.
function clampToLine(text, position) {
    position = Math.max(0, Math.min(text.length, position));
    var start = lineStartAt(text, position);
    var end = lineEndAt(text, position);
    return (position >= end && end > start)
        ? Math.max(start, stepBackward(text, end, 1))
        : position;
}

function nextWordStart(text, position, big) {
    var i = position;
    var cls = wordClassAt(text, i, big);
    if (cls !== 0)
        while (i < text.length && wordClassAt(text, i, big) === cls)
            i++;
    while (i < text.length && wordClassAt(text, i, big) === 0) {
        if (text[i] === "\n" && text[i + 1] === "\n")
            return i + 1;
        i++;
    }
    return i;
}

function prevWordStart(text, position, big) {
    var i = position - 1;
    while (i >= 0 && wordClassAt(text, i, big) === 0) {
        if (text[i] === "\n" && i > 0 && text[i - 1] === "\n")
            return i;
        i--;
    }
    var cls = wordClassAt(text, i, big);
    while (i > 0 && wordClassAt(text, i - 1, big) === cls)
        i--;
    return Math.max(0, i);
}

function wordEndForward(text, position, big) {
    var i = position + 1;
    while (i < text.length && wordClassAt(text, i, big) === 0)
        i++;
    if (i >= text.length)
        return Math.max(position, text.length - 1);
    var cls = wordClassAt(text, i, big);
    while (i + 1 < text.length && wordClassAt(text, i + 1, big) === cls)
        i++;
    return i;
}

function prevWordEnd(text, position, big) {
    var i = position - 1;
    var cls = wordClassAt(text, position, big);
    if (cls !== 0)
        while (i >= 0 && wordClassAt(text, i, big) === cls)
            i--;
    while (i >= 0 && wordClassAt(text, i, big) === 0)
        i--;
    return Math.max(0, i);
}

function wordClassAt(text, index, big) {
    var cls = charClass(text[index]);
    return big && cls === 2 ? 1 : cls;
}

// Paragraph boundaries are empty lines; { and } land on them like vim.
function paragraphForward(text, position) {
    var i = lineStartAt(text, position);
    var sawText = !lineIsEmpty(text, i);
    while (lineEndAt(text, i) < text.length) {
        i = lineEndAt(text, i) + 1;
        if (lineIsEmpty(text, i)) {
            if (sawText)
                return i;
        } else {
            sawText = true;
        }
    }
    return text.length;
}

function paragraphBackward(text, position) {
    var i = lineStartAt(text, position);
    var sawText = !lineIsEmpty(text, i);
    while (i > 0) {
        i = lineStartAt(text, i - 1);
        if (lineIsEmpty(text, i)) {
            if (sawText)
                return i;
        } else {
            sawText = true;
        }
    }
    return 0;
}

// A sentence ends at . ! or ? (plus closing quotes or brackets) followed by
// whitespace, matching vim's definition closely enough for prose.
var sentenceEnd = /[.!?][)\]"'’]*(?=\s|$)/g;

function sentenceForward(text, position) {
    sentenceEnd.lastIndex = position;
    var match = sentenceEnd.exec(text);
    var paragraph = paragraphForward(text, position);
    if (!match || match.index >= paragraph)
        return paragraph;
    var i = match.index + match[0].length;
    while (i < text.length && /\s/.test(text[i]))
        i++;
    return Math.min(i, paragraph);
}

function sentenceBackward(text, position) {
    var from = paragraphBackward(text, position);
    var starts = [firstNonBlankAt(text, from === 0 ? 0 : from + 1)];
    sentenceEnd.lastIndex = from;
    var match;
    while ((match = sentenceEnd.exec(text)) !== null) {
        var i = match.index + match[0].length;
        while (i < text.length && /\s/.test(text[i]))
            i++;
        if (i >= position)
            break;
        starts.push(i);
    }
    for (var s = starts.length - 1; s >= 0; s--) {
        if (starts[s] < position)
            return starts[s];
    }
    return Math.max(0, from);
}

function findInLine(text, position, command, character, count, repeated) {
    var start = lineStartAt(text, position);
    var end = lineEndAt(text, position);
    var forward = command === "f" || command === "t";
    var i = position;
    for (var n = 0; n < count; n++) {
        var probe = forward ? i + 1 : i - 1;
        // A repeated t/T would land where the cursor already is, so it skips
        // an adjacent match; a fresh press does not.
        if (repeated && command === "t" && text[probe] === character)
            probe = i + 2;
        else if (repeated && command === "T" && text[probe] === character)
            probe = i - 2;
        i = forward ? text.indexOf(character, probe)
                    : text.lastIndexOf(character, probe);
        if (i === -1 || i < start || i >= end)
            return -1;
    }
    if (command === "t")
        return i - 1;
    if (command === "T")
        return i + 1;
    return i;
}

function positionOfLine(text, lineNumber) {
    var position = 0;
    for (var line = 1; line < lineNumber && position <= text.length; line++) {
        var end = text.indexOf("\n", position);
        if (end === -1)
            return firstNonBlankAt(text, position);
        position = end + 1;
    }
    return firstNonBlankAt(text, position);
}

function lastLineStart(text) {
    return lineStartAt(text, text.length);
}

// Motions that only make sense against the laid-out text move by display
// line, the community-standard adaptation for soft-wrapped prose. The
// document's line spacing leaves leading between lines where positionAt
// resolves columns poorly, so scan for the neighboring line and then read
// the column at that line's vertical center. Stopping when no new line is
// found keeps the cursor put at the edges of the document.
function displayLinePosition(editor, fromPosition, direction, count, goalX) {
    var x = goalX >= 0 ? goalX : editor.positionToRectangle(fromPosition).x;
    var position = fromPosition;
    for (var i = 0; i < count; i++) {
        var rect = editor.positionToRectangle(position);
        var step = Math.max(2, rect.height / 2);
        var found = -1;
        for (var probe = step; probe <= rect.height * 4; probe += step) {
            var y = direction > 0 ? rect.y + rect.height + probe : rect.y - probe;
            if (y < 0)
                break;
            var lineRect = editor.positionToRectangle(editor.positionAt(x, y));
            if (lineRect.y !== rect.y) {
                found = editor.positionAt(x, lineRect.y + lineRect.height / 2);
                break;
            }
        }
        if (found < 0)
            break;
        position = found;
    }
    return { position: position, goalX: x };
}

function resetPending(vim) {
    vim.countText = "";
    vim.opCountText = "";
    vim.op = "";
    vim.prefix = "";
    vim.awaitingChar = "";
    updatePendingLabel(vim);
}

function updatePendingLabel(vim) {
    vim.pending = vim.countText + vim.op + vim.opCountText + vim.prefix
        + vim.awaitingChar;
}

function pendingCount(vim) {
    var before = vim.countText === "" ? 1 : parseInt(vim.countText, 10);
    var after = vim.opCountText === "" ? 1 : parseInt(vim.opCountText, 10);
    return before * after;
}

function hasCount(vim) {
    return vim.countText !== "" || vim.opCountText !== "";
}

function enterInsert(vim, editor) {
    if (!vim.replaying)
        vim.pendingChangeKeys = vim.keyLog.slice();
    vim.mode = "insert";
    vim.insertStart = editor.cursorPosition;
    resetPending(vim);
}

// Leaving insert steps the cursor back onto the last typed character and
// captures the session's text so `.` can replay the whole change.
function leaveInsert(vim, editor) {
    var position = editor.cursorPosition;
    if (!vim.replaying && vim.pendingChangeKeys && vim.pendingChangeKeys.length > 0) {
        var typed = position > vim.insertStart
            ? editor.text.slice(vim.insertStart, position)
            : "";
        vim.lastChange = { keys: vim.pendingChangeKeys, insert: typed };
    }
    vim.pendingChangeKeys = null;
    vim.mode = "normal";
    var text = editor.text;
    var start = lineStartAt(text, position);
    editor.cursorPosition = clampToLine(text, position > start ? position - 1 : position);
    resetPending(vim);
}

function enterVisual(vim, editor, linewise) {
    vim.mode = linewise ? "visualLine" : "visual";
    vim.anchor = editor.cursorPosition;
    vim.visualPos = editor.cursorPosition;
    updateVisualSelection(vim, editor);
    resetPending(vim);
}

// The visual selection always includes the character under the cursor.
function updateVisualSelection(vim, editor) {
    var text = editor.text;
    var start = Math.min(vim.anchor, vim.visualPos);
    var end = Math.max(vim.anchor, vim.visualPos);
    if (vim.mode === "visualLine")
        editor.select(lineStartAt(text, start),
                      Math.min(text.length, lineEndAt(text, end) + 1));
    else
        editor.select(start, Math.min(text.length, end + 1));
}

function leaveVisual(vim, editor) {
    editor.deselect();
    editor.cursorPosition = clampToLine(editor.text, vim.visualPos);
    vim.mode = "normal";
    resetPending(vim);
}

function moveTo(vim, editor, position, keepGoal) {
    if (!keepGoal)
        vim.goalX = -1;
    if (vim.mode === "normal") {
        editor.cursorPosition = clampToLine(editor.text, position);
    } else {
        vim.visualPos = clampToLine(editor.text, position);
        updateVisualSelection(vim, editor);
    }
}

function recordChange(vim) {
    if (!vim.replaying && vim.mode !== "visual" && vim.mode !== "visualLine"
            && vim.keyLog.length > 0)
        vim.lastChange = { keys: vim.keyLog.slice(), insert: null };
}

// Linewise register text carries a trailing newline, which is also how
// pastes tell the two paste shapes apart — even across other applications.
function pasteRegister(vim, editor, win, before, count) {
    var payload = win.vimClipboardText();
    if (payload === "")
        return;
    payload = payload.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
    var linewise = payload[payload.length - 1] === "\n";
    payload = payload.repeat(Math.min(count,
        Math.max(1, Math.floor(1000000 / payload.length))));
    var text = editor.text;
    var position = editor.cursorPosition;
    if (linewise) {
        var at = before ? lineStartAt(text, position) : lineEndAt(text, position) + 1;
        if (at > text.length) {
            editor.insert(text.length, "\n" + payload.slice(0, -1));
            editor.cursorPosition = firstNonBlankAt(editor.text, text.length + 1);
        } else {
            editor.insert(at, payload);
            editor.cursorPosition = firstNonBlankAt(editor.text, at);
        }
    } else {
        var insertAt = before ? position
                              : Math.min(position + 1, lineEndAt(text, position));
        editor.insert(insertAt, payload);
        editor.cursorPosition = payload.indexOf("\n") !== -1
            ? insertAt
            : clampToLine(editor.text, insertAt + payload.length - 1);
    }
    recordChange(vim);
}

function replaceCharacters(vim, editor, win, character, count) {
    if (character === "\r")
        character = "\n";
    var text = editor.text;
    var position = editor.cursorPosition;
    var end = stepForward(text, position, count);
    if (end > lineEndAt(text, position))
        return;
    win.vimReplace(position, end, character.repeat(count));
    editor.cursorPosition = character === "\n"
        ? position + count
        : clampToLine(editor.text, position + count - 1);
    recordChange(vim);
}

function toggleCase(vim, editor, win, count) {
    var text = editor.text;
    var position = editor.cursorPosition;
    var end = Math.min(stepForward(text, position, count), lineEndAt(text, position));
    if (end <= position)
        return;
    var flipped = text.slice(position, end).replace(/./g, function(character) {
        var lower = character.toLowerCase();
        return character === lower ? character.toUpperCase() : lower;
    });
    win.vimReplace(position, end, flipped);
    editor.cursorPosition = clampToLine(editor.text, end);
    recordChange(vim);
}

function joinLines(vim, editor, win, count) {
    for (var joins = Math.max(1, count - 1); joins > 0; joins--) {
        var text = editor.text;
        var position = editor.cursorPosition;
        var end = lineEndAt(text, position);
        if (end >= text.length)
            break;
        var next = end + 1;
        while (next < text.length && (text[next] === " " || text[next] === "\t"))
            next++;
        var spaced = end > lineStartAt(text, position)
            && next < text.length && text[next] !== "\n"
            && text[end - 1] !== " " && text[end - 1] !== "\t";
        win.vimReplace(end, next, spaced ? " " : "");
        editor.cursorPosition = clampToLine(editor.text, end);
    }
    recordChange(vim);
}

// Runs the pending operator over [start, end); linewise ranges are widened
// to whole lines and yanked with the trailing newline that marks them.
function applyOperator(vim, editor, win, start, end, linewise) {
    var op = vim.op;
    var text = editor.text;
    if (linewise) {
        var low = Math.min(start, end);
        var high = Math.max(start, end);
        start = lineStartAt(text, low);
        end = Math.min(text.length, lineEndAt(text, high) + 1);
    } else if (end < start) {
        var swap = start;
        start = end;
        end = swap;
    }
    // Deleting the last line takes the newline before it, which also lets a
    // trailing empty line be deleted even though its own range is empty.
    var atEof = linewise && op === "d" && end === text.length && start > 0;
    if (end <= start && op !== "c" && !atEof) {
        resetPending(vim);
        return;
    }
    var payload = text.slice(start, end);
    if (linewise && payload[payload.length - 1] !== "\n")
        payload += "\n";
    if (end > start || linewise)
        win.vimYank(payload);

    if (op === "y") {
        editor.cursorPosition = clampToLine(text, linewise ? editor.cursorPosition : start);
    } else if (op === "d") {
        editor.remove(atEof ? start - 1 : start, end);
        var remaining = editor.text;
        var landing = clampToLine(remaining, Math.min(start, remaining.length));
        editor.cursorPosition = linewise ? firstNonBlankAt(remaining, landing) : landing;
        recordChange(vim);
    } else if (op === "c") {
        if (linewise && end > start && text[end - 1] === "\n")
            win.vimReplace(start, end, "\n");
        else if (end > start)
            editor.remove(start, end);
        editor.cursorPosition = start;
        enterInsert(vim, editor);
        return;
    }
    resetPending(vim);
}

// Where a motion lands, and whether an operator over it takes the final
// character (inclusive) or whole lines. j, k, and gg are handled separately.
function motionTarget(vim, editor, ch, count, argument) {
    var text = editor.text;
    var position = vim.mode === "normal" ? editor.cursorPosition : vim.visualPos;
    var target;
    var stepped;
    switch (ch) {
    case "h":
        return { pos: Math.max(lineStartAt(text, position),
                               stepBackward(text, position, count)) };
    case "l":
    case " ":
        return { pos: Math.min(lineEndAt(text, position),
                               stepForward(text, position, count)) };
    case "0":
        return { pos: lineStartAt(text, position) };
    case "^":
        return { pos: firstNonBlankAt(text, position) };
    case "$":
        target = position;
        for (var lines = count; lines > 1 && lineEndAt(text, target) < text.length; lines--)
            target = lineEndAt(text, target) + 1;
        return { pos: lineEndAt(text, target), goalX: Number.MAX_VALUE };
    case "w":
    case "W":
        target = position;
        for (var w = 0; w < count; w++) {
            stepped = nextWordStart(text, target, ch === "W");
            if (stepped === target)
                break;
            target = stepped;
        }
        return { pos: target };
    case "b":
    case "B":
        target = position;
        for (var b = 0; b < count; b++) {
            stepped = prevWordStart(text, target, ch === "B");
            if (stepped === target)
                break;
            target = stepped;
        }
        return { pos: target };
    case "e":
    case "E":
        target = position;
        for (var e = 0; e < count; e++) {
            stepped = wordEndForward(text, target, ch === "E");
            if (stepped === target)
                break;
            target = stepped;
        }
        return { pos: target, inclusive: true };
    case "}":
        target = position;
        for (var pf = 0; pf < count; pf++) {
            stepped = paragraphForward(text, target);
            if (stepped === target)
                break;
            target = stepped;
        }
        return { pos: target };
    case "{":
        target = position;
        for (var pb = 0; pb < count; pb++) {
            stepped = paragraphBackward(text, target);
            if (stepped === target)
                break;
            target = stepped;
        }
        return { pos: target };
    case ")":
        target = position;
        for (var sf = 0; sf < count; sf++) {
            stepped = sentenceForward(text, target);
            if (stepped === target)
                break;
            target = stepped;
        }
        return { pos: target };
    case "(":
        target = position;
        for (var sb = 0; sb < count; sb++) {
            stepped = sentenceBackward(text, target);
            if (stepped === target)
                break;
            target = stepped;
        }
        return { pos: target };
    case "G":
        return { pos: hasCount(vim) ? positionOfLine(text, count)
                                    : firstNonBlankAt(text, lastLineStart(text)),
                 linewise: true };
    case "\r":
        target = position;
        for (var down = 0; down < count && lineEndAt(text, target) < text.length; down++)
            target = lineEndAt(text, target) + 1;
        if (lineStartAt(text, target) === lineStartAt(text, position))
            return null;
        return { pos: firstNonBlankAt(text, target), linewise: true };
    case "f":
    case "F":
    case "t":
    case "T":
        target = findInLine(text, position, ch, argument, count, false);
        if (target === -1)
            return null;
        vim.lastFindCmd = ch;
        vim.lastFindChar = argument;
        return { pos: target, inclusive: ch === "f" || ch === "t" };
    case ";":
    case ",":
        if (vim.lastFindCmd === "")
            return null;
        var command = vim.lastFindCmd;
        if (ch === ",")
            command = { f: "F", F: "f", t: "T", T: "t" }[command];
        target = findInLine(text, position, command, vim.lastFindChar, count, true);
        if (target === -1)
            return null;
        return { pos: target, inclusive: command === "f" || command === "t" };
    }
    return undefined;
}

function lineSpanEnd(text, position, count) {
    var target = position;
    for (; count > 1 && lineEndAt(text, target) < text.length; count--)
        target = lineEndAt(text, target) + 1;
    return target;
}

function operateWithMotion(vim, editor, win, ch, count, argument) {
    var text = editor.text;
    var position = editor.cursorPosition;
    // cw acts like ce when the cursor sits on a word (:h cw).
    if (vim.op === "c" && (ch === "w" || ch === "W") && charClass(text[position]) !== 0)
        ch = ch === "w" ? "e" : "E";
    var motion = motionTarget(vim, editor, ch, count, argument);
    if (!motion || (!motion.linewise && motion.pos === position)) {
        // A motion that cannot move fails the whole operation.
        resetPending(vim);
        return;
    }
    if (motion.linewise) {
        applyOperator(vim, editor, win, position, motion.pos, true);
        return;
    }
    var start = position;
    var end = motion.pos;
    var linewise = false;
    if (end >= start) {
        if (motion.inclusive) {
            end = Math.min(text.length, end + 1);
        } else if (end > 0 && end === lineStartAt(text, end)
                   && end > lineEndAt(text, start)) {
            // An exclusive motion ending in column 1 stops at the end of the
            // previous line; from at or before the first word it turns
            // linewise (:h exclusive-linewise). Word motions are exempt: dw
            // on the last word of a line only deletes to the line's end.
            end--;
            linewise = ch !== "w" && ch !== "W"
                && start <= firstNonBlankAt(text, start);
        }
    } else {
        start = motion.pos;
        end = position;
    }
    applyOperator(vim, editor, win, start, end, linewise);
}

var objectPairs = { "(": "()", ")": "()", b: "()", "[": "[]", "]": "[]",
                    "{": "{}", "}": "{}", B: "{}" };

function textObjectRange(text, position, around, kind) {
    if (kind === "w" || kind === "W") {
        var big = kind === "W";
        var cls = wordClassAt(text, position, big);
        var start = position;
        var end = position;
        while (start > 0 && text[start - 1] !== "\n"
               && wordClassAt(text, start - 1, big) === cls)
            start--;
        while (end < text.length && text[end] !== "\n"
               && wordClassAt(text, end, big) === cls)
            end++;
        if (around) {
            var trailing = end;
            while (trailing < text.length && (text[trailing] === " " || text[trailing] === "\t"))
                trailing++;
            if (trailing > end)
                end = trailing;
            else
                while (start > 0 && (text[start - 1] === " " || text[start - 1] === "\t"))
                    start--;
        }
        return { start: start, end: end, linewise: false };
    }
    if (kind === "p") {
        var first = lineStartAt(text, position);
        var empty = lineIsEmpty(text, first);
        var last = first;
        while (first > 0 && lineIsEmpty(text, lineStartAt(text, first - 1)) === empty)
            first = lineStartAt(text, first - 1);
        while (lineEndAt(text, last) < text.length
               && lineIsEmpty(text, lineEndAt(text, last) + 1) === empty)
            last = lineEndAt(text, last) + 1;
        var stop = Math.min(text.length, lineEndAt(text, last) + 1);
        if (around) {
            var grown = stop;
            while (grown < text.length && lineIsEmpty(text, grown))
                grown = lineEndAt(text, grown) + 1;
            if (grown > stop)
                stop = Math.min(text.length, grown);
            else
                while (first > 0 && lineIsEmpty(text, lineStartAt(text, first - 1)))
                    first = lineStartAt(text, first - 1);
        }
        return { start: first, end: stop, linewise: true };
    }
    if (kind === "\"" || kind === "'") {
        var open = -1;
        var lineEnd = lineEndAt(text, position);
        for (var q = lineStartAt(text, position); q < lineEnd; q++) {
            if (text[q] !== kind)
                continue;
            if (open === -1) {
                open = q;
            } else if (q >= position) {
                return around ? { start: open, end: q + 1 }
                              : { start: open + 1, end: q };
            } else {
                open = -1;
            }
        }
        return null;
    }
    var pair = objectPairs[kind];
    if (!pair)
        return null;
    var depth = 0;
    var openPos = -1;
    for (var backward = position; backward >= 0; backward--) {
        if (text[backward] === pair[1] && backward !== position) {
            depth++;
        } else if (text[backward] === pair[0]) {
            if (depth === 0) {
                openPos = backward;
                break;
            }
            depth--;
        }
    }
    if (openPos === -1)
        return null;
    depth = 0;
    for (var forward = openPos + 1; forward < text.length; forward++) {
        if (text[forward] === pair[0]) {
            depth++;
        } else if (text[forward] === pair[1]) {
            if (depth === 0)
                return around ? { start: openPos, end: forward + 1 }
                              : { start: openPos + 1, end: forward };
            depth--;
        }
    }
    return null;
}

function visualOperate(vim, editor, win, op) {
    var linewise = vim.mode === "visualLine";
    var start = editor.selectionStart;
    var end = editor.selectionEnd;
    vim.mode = "normal";
    editor.deselect();
    vim.keyLog = [];
    vim.op = op;
    applyOperator(vim, editor, win, start,
                  linewise ? Math.max(start, end - 1) : end, linewise);
}

function visualPaste(vim, editor, win) {
    var payload = win.vimClipboardText();
    var linewise = vim.mode === "visualLine";
    var start = editor.selectionStart;
    var end = editor.selectionEnd;
    vim.mode = "normal";
    editor.deselect();
    resetPending(vim);
    if (payload === "")
        return;
    payload = payload.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
    if (!linewise && payload[payload.length - 1] === "\n")
        payload = payload.slice(0, -1);
    if (linewise && payload[payload.length - 1] !== "\n" && end < editor.text.length)
        payload += "\n";
    win.vimReplace(start, end, payload);
    editor.cursorPosition = clampToLine(editor.text, start + payload.length - 1);
}

function repeatChange(vim, editor, win) {
    var change = vim.lastChange;
    if (!change)
        return;
    vim.replaying = true;
    resetPending(vim);
    for (var i = 0; i < change.keys.length; i++)
        normalKey(vim, editor, win, change.keys[i]);
    if (vim.mode === "insert") {
        if (change.insert) {
            var at = editor.cursorPosition;
            editor.insert(at, change.insert);
            editor.cursorPosition = at + change.insert.length;
        }
        leaveInsert(vim, editor);
    }
    vim.replaying = false;
}

// The one entry point QML calls. Returns whether the key was consumed;
// insert mode passes everything but Escape through to the editor.
function handleKey(vim, editor, win, event) {
    if (editor.inputMethodComposing)
        return false;
    if (event.key === Qt.Key_Escape) {
        if (vim.mode === "insert")
            leaveInsert(vim, editor);
        else if (vim.mode === "visual" || vim.mode === "visualLine")
            leaveVisual(vim, editor);
        else {
            resetPending(vim);
            editor.deselect();
        }
        return true;
    }
    if (vim.mode === "insert")
        return false;
    if (event.modifiers & Qt.ControlModifier) {
        // Any control chord aborts a pending operator or count, like vim.
        if (event.key !== Qt.Key_Control)
            resetPending(vim);
        if (event.key === Qt.Key_R) {
            editor.redo();
            editor.cursorPosition = clampToLine(editor.text, editor.cursorPosition);
            return true;
        }
        if ((event.key === Qt.Key_D || event.key === Qt.Key_U) && vim.mode === "normal") {
            editor.movePage(event.key === Qt.Key_D ? 1 : -1, false);
            return true;
        }
        return false;
    }
    if (event.modifiers & (Qt.AltModifier | Qt.MetaModifier))
        return false;
    var ch = event.text;
    if (event.key === Qt.Key_Backspace)
        ch = "h";
    else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
        ch = "\r";
    else if (event.key === Qt.Key_Left)
        ch = "h";
    else if (event.key === Qt.Key_Right)
        ch = "l";
    else if (event.key === Qt.Key_Up)
        ch = "k";
    else if (event.key === Qt.Key_Down)
        ch = "j";
    if (!ch) {
        // Unmapped non-modifier keys abort pending state; bare modifier
        // presses mid-chord must not.
        if (event.key !== Qt.Key_Shift && event.key !== Qt.Key_Alt
                && event.key !== Qt.Key_Meta && event.key !== Qt.Key_AltGr
                && event.key !== Qt.Key_CapsLock)
            resetPending(vim);
        return false;
    }
    return normalKey(vim, editor, win, ch);
}

// Idle-to-idle key sequences are logged so `.` can replay the last change.
function normalKey(vim, editor, win, ch) {
    var recording = vim.mode === "normal" && !vim.replaying;
    if (recording && vim.countText === "" && vim.opCountText === "" && vim.op === ""
            && vim.prefix === "" && vim.awaitingChar === "")
        vim.keyLog = [];
    if (recording)
        vim.keyLog.push(ch);
    var handled = dispatch(vim, editor, win, ch);
    updatePendingLabel(vim);
    return handled;
}

function dispatch(vim, editor, win, ch) {
    var count = pendingCount(vim);
    var visual = vim.mode === "visual" || vim.mode === "visualLine";

    if (vim.awaitingChar !== "") {
        var awaited = vim.awaitingChar;
        vim.awaitingChar = "";
        if (awaited === "r") {
            replaceCharacters(vim, editor, win, ch, count);
            resetPending(vim);
        } else if (vim.op !== "") {
            operateWithMotion(vim, editor, win, awaited, count, ch);
        } else {
            var found = motionTarget(vim, editor, awaited, count, ch);
            if (found)
                moveTo(vim, editor, found.pos, false);
            resetPending(vim);
        }
        return true;
    }

    if (vim.prefix === "i" || vim.prefix === "a") {
        var around = vim.prefix === "a";
        vim.prefix = "";
        var objectText = editor.text;
        var object = textObjectRange(objectText,
                                     visual ? vim.visualPos : editor.cursorPosition,
                                     around, ch);
        if (!object) {
            resetPending(vim);
            return true;
        }
        if (visual) {
            vim.anchor = object.start;
            vim.visualPos = Math.max(object.start,
                                     stepBackward(objectText, object.end, 1));
            if (object.linewise)
                vim.mode = "visualLine";
            updateVisualSelection(vim, editor);
            resetPending(vim);
            return true;
        }
        applyOperator(vim, editor, win, object.start,
                      object.linewise ? Math.max(object.start, object.end - 1)
                                      : object.end,
                      object.linewise);
        return true;
    }

    if (vim.prefix === "g") {
        vim.prefix = "";
        var text = editor.text;
        if (ch === "g") {
            var top = hasCount(vim) ? positionOfLine(text, count)
                                    : firstNonBlankAt(text, 0);
            if (vim.op !== "") {
                applyOperator(vim, editor, win, editor.cursorPosition, top, true);
            } else {
                moveTo(vim, editor, top, false);
                resetPending(vim);
            }
        } else if (ch === "e" || ch === "E") {
            var backTo = visual ? vim.visualPos : editor.cursorPosition;
            for (var ge = 0; ge < count; ge++) {
                var geStep = prevWordEnd(text, backTo, ch === "E");
                if (geStep === backTo)
                    break;
                backTo = geStep;
            }
            if (vim.op !== "") {
                // ge is an inclusive motion; take the cursor character too.
                applyOperator(vim, editor, win, backTo,
                              stepForward(text, editor.cursorPosition, 1), false);
            } else {
                moveTo(vim, editor, backTo, false);
                resetPending(vim);
            }
        } else {
            resetPending(vim);
        }
        return true;
    }

    if (ch >= "0" && ch <= "9"
            && !(ch === "0" && (vim.op === "" ? vim.countText === ""
                                              : vim.opCountText === ""))) {
        if (vim.op === "")
            vim.countText += ch;
        else
            vim.opCountText += ch;
        return true;
    }

    if (ch === "d" || ch === "c" || ch === "y") {
        if (visual) {
            visualOperate(vim, editor, win, ch);
            return true;
        }
        if (vim.op === ch) {
            var span = editor.cursorPosition;
            applyOperator(vim, editor, win, span,
                          lineSpanEnd(editor.text, span, count), true);
            return true;
        }
        if (vim.op !== "") {
            resetPending(vim);
            return true;
        }
        if (editor.selectionStart !== editor.selectionEnd) {
            // A mouse selection stands in for a visual range.
            var selStart = editor.selectionStart;
            var selEnd = editor.selectionEnd;
            editor.deselect();
            vim.keyLog = [];
            vim.op = ch;
            applyOperator(vim, editor, win, selStart, selEnd, false);
            return true;
        }
        vim.op = ch;
        return true;
    }

    if (ch === "g") {
        vim.prefix = "g";
        return true;
    }
    if (ch === "f" || ch === "F" || ch === "t" || ch === "T"
            || (ch === "r" && !visual && vim.op === "")) {
        vim.awaitingChar = ch;
        return true;
    }

    if (vim.op !== "") {
        if (ch === "i" || ch === "a") {
            vim.prefix = ch;
            return true;
        }
        if (ch === "j" || ch === "k") {
            var opText = editor.text;
            var from = editor.cursorPosition;
            var to = from;
            for (var opLines = 0; opLines < count; opLines++) {
                if (ch === "j") {
                    var lineBreak = lineEndAt(opText, to);
                    if (lineBreak >= opText.length)
                        break;
                    to = lineBreak + 1;
                } else {
                    var lineTop = lineStartAt(opText, to);
                    if (lineTop === 0)
                        break;
                    to = lineTop - 1;
                }
            }
            // A line motion that cannot move fails the whole operation.
            if (lineStartAt(opText, to) === lineStartAt(opText, from)) {
                resetPending(vim);
                return true;
            }
            applyOperator(vim, editor, win, from, to, true);
            return true;
        }
        operateWithMotion(vim, editor, win, ch, count);
        return true;
    }

    if (visual) {
        if (ch === "i" || ch === "a") {
            vim.prefix = ch;
            return true;
        }
        if (ch === "x") {
            visualOperate(vim, editor, win, "d");
            return true;
        }
        if (ch === "p") {
            visualPaste(vim, editor, win);
            return true;
        }
        if (ch === "o") {
            var swap = vim.anchor;
            vim.anchor = vim.visualPos;
            vim.visualPos = swap;
            updateVisualSelection(vim, editor);
            return true;
        }
        if (ch === "v" || ch === "V") {
            var wantsLine = ch === "V";
            if ((vim.mode === "visualLine") === wantsLine) {
                leaveVisual(vim, editor);
            } else {
                vim.mode = wantsLine ? "visualLine" : "visual";
                updateVisualSelection(vim, editor);
            }
            return true;
        }
    }

    // j and k walk display lines so wrapped prose moves the way it reads.
    if (ch === "j" || ch === "k") {
        var origin = visual ? vim.visualPos : editor.cursorPosition;
        var moved = displayLinePosition(editor, origin, ch === "j" ? 1 : -1,
                                        count, vim.goalX);
        moveTo(vim, editor, moved.position, true);
        vim.goalX = moved.goalX;
        resetPending(vim);
        return true;
    }

    var motion = motionTarget(vim, editor, ch, count);
    if (motion === null) {
        resetPending(vim);
        return true;
    }
    if (motion !== undefined) {
        moveTo(vim, editor, motion.pos, false);
        if (motion.goalX !== undefined)
            vim.goalX = motion.goalX;
        resetPending(vim);
        return true;
    }

    if (visual) {
        resetPending(vim);
        return true;
    }

    return normalCommand(vim, editor, win, ch, count);
}

function normalCommand(vim, editor, win, ch, count) {
    var text = editor.text;
    var position = editor.cursorPosition;
    switch (ch) {
    case "i":
        enterInsert(vim, editor);
        return true;
    case "I":
        editor.cursorPosition = firstNonBlankAt(text, position);
        enterInsert(vim, editor);
        return true;
    case "a":
        editor.cursorPosition = Math.min(stepForward(text, position, 1),
                                         lineEndAt(text, position));
        enterInsert(vim, editor);
        return true;
    case "A":
        editor.cursorPosition = lineEndAt(text, position);
        enterInsert(vim, editor);
        return true;
    case "o":
        var below = lineEndAt(text, position);
        editor.insert(below, "\n");
        editor.cursorPosition = below + 1;
        enterInsert(vim, editor);
        return true;
    case "O":
        var above = lineStartAt(text, position);
        editor.insert(above, "\n");
        editor.cursorPosition = above;
        enterInsert(vim, editor);
        return true;
    case "v":
        enterVisual(vim, editor, false);
        return true;
    case "V":
        enterVisual(vim, editor, true);
        return true;
    case "x":
        vim.op = "d";
        applyOperator(vim, editor, win, position,
                      Math.min(stepForward(text, position, count),
                               lineEndAt(text, position)), false);
        return true;
    case "X":
        vim.op = "d";
        applyOperator(vim, editor, win,
                      Math.max(lineStartAt(text, position),
                               stepBackward(text, position, count)),
                      position, false);
        return true;
    case "D":
        vim.op = "d";
        applyOperator(vim, editor, win, position,
                      lineEndAt(text, lineSpanEnd(text, position, count)), false);
        return true;
    case "C":
        vim.op = "c";
        applyOperator(vim, editor, win, position,
                      lineEndAt(text, lineSpanEnd(text, position, count)), false);
        return true;
    case "Y":
        vim.op = "y";
        applyOperator(vim, editor, win, position,
                      lineSpanEnd(text, position, count), true);
        return true;
    case "s":
        vim.op = "c";
        applyOperator(vim, editor, win, position,
                      Math.min(stepForward(text, position, count),
                               lineEndAt(text, position)), false);
        return true;
    case "S":
        vim.op = "c";
        applyOperator(vim, editor, win, position,
                      lineSpanEnd(text, position, count), true);
        return true;
    case "p":
        pasteRegister(vim, editor, win, false, count);
        resetPending(vim);
        return true;
    case "P":
        pasteRegister(vim, editor, win, true, count);
        resetPending(vim);
        return true;
    case "J":
        joinLines(vim, editor, win, count);
        resetPending(vim);
        return true;
    case "~":
        toggleCase(vim, editor, win, count);
        resetPending(vim);
        return true;
    case "u":
        editor.undo();
        editor.cursorPosition = clampToLine(editor.text, editor.cursorPosition);
        resetPending(vim);
        return true;
    case ".":
        repeatChange(vim, editor, win);
        resetPending(vim);
        return true;
    case ":":
        win.openVimCommand();
        resetPending(vim);
        return true;
    case "/":
        win.openVimSearch();
        resetPending(vim);
        return true;
    case "n":
        win.moveSearch(1);
        resetPending(vim);
        return true;
    case "N":
        win.moveSearch(-1);
        resetPending(vim);
        return true;
    }
    // Unknown keys are swallowed, never typed into the document.
    resetPending(vim);
    return true;
}
