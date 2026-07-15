#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define MAX_UNDO 200
#define TAB_SIZE 4

typedef struct {
    char *chars;
    int len;
    int cap;
} Row;

typedef struct {
    Row *rows;
    int nrows;
    int cap;
} Buffer;

typedef enum { ACTION_NONE, ACTION_INSERT, ACTION_DELETE } ActionType;

typedef struct {
    char **stack;
    int top;
    int count;
} SnapStack;

typedef struct {
    Buffer buf;
    int cx, cy;
    int rowoff, coloff;
    char filename[512];
    int modified;
    SnapStack undo, redo;
    ActionType last_action;
    char status[256];
    int status_ticks;
    HANDLE hOut;
    HANDLE hIn;
    CHAR_INFO *screen;
    int screen_rows, screen_cols;
} Editor;

static Editor E;

void row_init(Row *r) {
    r->cap = 32;
    r->chars = malloc(r->cap);
    r->chars[0] = '\0';
    r->len = 0;
}

void row_ensure(Row *r, int needed) {
    if (needed + 1 > r->cap) {
        while (needed + 1 > r->cap) r->cap *= 2;
        r->chars = realloc(r->chars, r->cap);
    }
}

void row_insert_char(Row *r, int at, char c) {
    if (at < 0) at = 0;
    if (at > r->len) at = r->len;
    row_ensure(r, r->len + 1);
    memmove(&r->chars[at + 1], &r->chars[at], r->len - at + 1);
    r->chars[at] = c;
    r->len++;
}

void row_delete_char(Row *r, int at) {
    if (at < 0 || at >= r->len) return;
    memmove(&r->chars[at], &r->chars[at + 1], r->len - at);
    r->len--;
}

void row_append_str(Row *r, const char *s, int slen) {
    row_ensure(r, r->len + slen);
    memcpy(&r->chars[r->len], s, slen);
    r->len += slen;
    r->chars[r->len] = '\0';
}

void buffer_init(Buffer *b) {
    b->cap = 16;
    b->rows = malloc(sizeof(Row) * b->cap);
    b->nrows = 1;
    row_init(&b->rows[0]);
}

void buffer_ensure(Buffer *b, int needed) {
    if (needed > b->cap) {
        while (needed > b->cap) b->cap *= 2;
        b->rows = realloc(b->rows, sizeof(Row) * b->cap);
    }
}

void buffer_insert_row(Buffer *b, int at, const char *s, int slen) {
    buffer_ensure(b, b->nrows + 1);
    memmove(&b->rows[at + 1], &b->rows[at], sizeof(Row) * (b->nrows - at));
    row_init(&b->rows[at]);
    row_append_str(&b->rows[at], s, slen);
    b->nrows++;
}

void buffer_delete_row(Buffer *b, int at) {
    if (at < 0 || at >= b->nrows) return;
    free(b->rows[at].chars);
    memmove(&b->rows[at], &b->rows[at + 1], sizeof(Row) * (b->nrows - at - 1));
    b->nrows--;
}

void buffer_free(Buffer *b) {
    for (int i = 0; i < b->nrows; i++) free(b->rows[i].chars);
    free(b->rows);
}

char *buffer_serialize(Buffer *b, int *outlen) {
    int total = 0;
    for (int i = 0; i < b->nrows; i++) total += b->rows[i].len + 1;
    char *s = malloc(total + 1);
    int pos = 0;
    for (int i = 0; i < b->nrows; i++) {
        memcpy(&s[pos], b->rows[i].chars, b->rows[i].len);
        pos += b->rows[i].len;
        s[pos++] = '\n';
    }
    s[pos] = '\0';
    if (outlen) *outlen = pos;
    return s;
}

void buffer_from_string(Buffer *b, const char *s, int slen) {
    buffer_free(b);
    b->cap = 16;
    b->rows = malloc(sizeof(Row) * b->cap);
    b->nrows = 0;
    int start = 0;
    for (int i = 0; i <= slen; i++) {
        if (i == slen || s[i] == '\n') {
            int end = i;
            if (end > start && s[end - 1] == '\r') end--;
            buffer_ensure(b, b->nrows + 1);
            row_init(&b->rows[b->nrows]);
            row_append_str(&b->rows[b->nrows], &s[start], end - start);
            b->nrows++;
            start = i + 1;
        }
    }
    if (b->nrows == 0) {
        row_init(&b->rows[0]);
        b->nrows = 1;
    }
}

void snap_push(SnapStack *st, const char *text) {
    if (st->count == MAX_UNDO) {
        free(st->stack[0]);
        memmove(&st->stack[0], &st->stack[1], sizeof(char *) * (MAX_UNDO - 1));
        st->count--;
    }
    st->stack[st->count++] = _strdup(text);
}

char *snap_pop(SnapStack *st) {
    if (st->count == 0) return NULL;
    return st->stack[--st->count];
}

void snap_clear(SnapStack *st) {
    for (int i = 0; i < st->count; i++) free(st->stack[i]);
    st->count = 0;
}

void set_status(const char *msg) {
    strncpy(E.status, msg, sizeof(E.status) - 1);
    E.status[sizeof(E.status) - 1] = '\0';
    E.status_ticks = 40;
}

void push_undo_snapshot() {
    int len;
    char *s = buffer_serialize(&E.buf, &len);
    snap_push(&E.undo, s);
    free(s);
    snap_clear(&E.redo);
}

void editor_will_edit(ActionType type) {
    if (E.last_action != type) {
        push_undo_snapshot();
        E.last_action = type;
    }
    E.modified = 1;
}

void editor_break_run() {
    E.last_action = ACTION_NONE;
}

void do_undo() {
    if (E.undo.count == 0) {
        set_status("No undo");
        return;
    }
    int len;
    char *current = buffer_serialize(&E.buf, &len);
    snap_push(&E.redo, current);
    free(current);
    char *prev = snap_pop(&E.undo);
    buffer_from_string(&E.buf, prev, (int)strlen(prev));
    free(prev);
    if (E.cy >= E.buf.nrows) E.cy = E.buf.nrows - 1;
    if (E.cx > E.buf.rows[E.cy].len) E.cx = E.buf.rows[E.cy].len;
    editor_break_run();
    set_status("Undo");
}

void do_redo() {
    if (E.redo.count == 0) {
        set_status("No redo");
        return;
    }
    int len;
    char *current = buffer_serialize(&E.buf, &len);
    snap_push(&E.undo, current);
    free(current);
    char *next = snap_pop(&E.redo);
    buffer_from_string(&E.buf, next, (int)strlen(next));
    free(next);
    if (E.cy >= E.buf.nrows) E.cy = E.buf.nrows - 1;
    if (E.cx > E.buf.rows[E.cy].len) E.cx = E.buf.rows[E.cy].len;
    editor_break_run();
    set_status("Redo");
}

void insert_char(int c) {
    editor_will_edit(ACTION_INSERT);
    row_insert_char(&E.buf.rows[E.cy], E.cx, (char)c);
    E.cx++;
}

void insert_newline() {
    editor_will_edit(ACTION_INSERT);
    Row *r = &E.buf.rows[E.cy];
    int taillen = r->len - E.cx;
    char *tail = malloc(taillen + 1);
    memcpy(tail, &r->chars[E.cx], taillen);
    tail[taillen] = '\0';
    r->len = E.cx;
    r->chars[r->len] = '\0';
    buffer_insert_row(&E.buf, E.cy + 1, tail, taillen);
    free(tail);
    E.cy++;
    E.cx = 0;
}

void delete_backward() {
    if (E.cx == 0 && E.cy == 0) return;
    editor_will_edit(ACTION_DELETE);
    if (E.cx > 0) {
        row_delete_char(&E.buf.rows[E.cy], E.cx - 1);
        E.cx--;
    } else {
        Row *prev = &E.buf.rows[E.cy - 1];
        int prevlen = prev->len;
        row_append_str(prev, E.buf.rows[E.cy].chars, E.buf.rows[E.cy].len);
        buffer_delete_row(&E.buf, E.cy);
        E.cy--;
        E.cx = prevlen;
    }
}

void delete_forward() {
    Row *r = &E.buf.rows[E.cy];
    if (E.cx == r->len && E.cy == E.buf.nrows - 1) return;
    editor_will_edit(ACTION_DELETE);
    if (E.cx < r->len) {
        row_delete_char(r, E.cx);
    } else {
        row_append_str(r, E.buf.rows[E.cy + 1].chars, E.buf.rows[E.cy + 1].len);
        buffer_delete_row(&E.buf, E.cy + 1);
    }
}

void ensure_txt_extension(char *name) {
    int len = (int)strlen(name);
    if (len < 4 || _stricmp(&name[len - 4], ".txt") != 0) {
        strcat(name, ".txt");
    }
}

int save_file() {
    if (strlen(E.filename) == 0) return 0;
    FILE *f = fopen(E.filename, "wb");
    if (!f) {
        set_status("Save failed");
        return -1;
    }
    for (int i = 0; i < E.buf.nrows; i++) {
        fwrite(E.buf.rows[i].chars, 1, E.buf.rows[i].len, f);
        fputc('\r', f);
        fputc('\n', f);
    }
    fclose(f);
    E.modified = 0;
    set_status("Saved");
    return 0;
}

void load_file(const char *name) {
    strncpy(E.filename, name, sizeof(E.filename) - 1);
    ensure_txt_extension(E.filename);
    FILE *f = fopen(E.filename, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(size + 1);
    size_t r = fread(data, 1, size, f);
    data[r] = '\0';
    fclose(f);
    buffer_from_string(&E.buf, data, (int)r);
    free(data);
}

void get_console_size(int *rows, int *cols) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(E.hOut, &csbi);
    *cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    *rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
}

void ensure_screen_buffer(int rows, int cols) {
    if (E.screen_rows == rows && E.screen_cols == cols && E.screen) return;
    free(E.screen);
    E.screen = malloc(sizeof(CHAR_INFO) * rows * cols);
    E.screen_rows = rows;
    E.screen_cols = cols;
}

void put_cell(int row, int col, char ch, WORD attr) {
    if (row < 0 || row >= E.screen_rows || col < 0 || col >= E.screen_cols) return;
    CHAR_INFO *cell = &E.screen[row * E.screen_cols + col];
    cell->Char.AsciiChar = ch;
    cell->Attributes = attr;
}

void put_str(int row, int col, const char *s, int len, WORD attr) {
    for (int i = 0; i < len; i++) {
        put_cell(row, col + i, s[i], attr);
    }
}

void fill_row(int row, WORD attr) {
    for (int c = 0; c < E.screen_cols; c++) put_cell(row, c, ' ', attr);
}

void draw_screen() {
    int rows, cols;
    get_console_size(&rows, &cols);
    ensure_screen_buffer(rows, cols);

    int text_rows = rows - 2;
    int linenum_width = 6;

    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + text_rows) E.rowoff = E.cy - text_rows + 1;
    if (E.cx < E.coloff) E.coloff = E.cx;
    if (E.cx >= E.coloff + (cols - linenum_width)) E.coloff = E.cx - (cols - linenum_width) + 1;

    WORD title_attr = BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    WORD linenum_attr = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    WORD text_attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    WORD status_attr = BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;

    fill_row(0, title_attr);
    char title[768];
    snprintf(title, sizeof(title), " fishedit  -  %s%s",
             strlen(E.filename) ? E.filename : "no name",
             E.modified ? " *" : "");
    put_str(0, 0, title, (int)strlen(title), title_attr);

    for (int y = 0; y < text_rows; y++) {
        int filerow = y + E.rowoff;
        fill_row(y + 1, text_attr);
        if (filerow < E.buf.nrows) {
            char linebuf[16];
            snprintf(linebuf, sizeof(linebuf), "%4d |", filerow + 1);
            put_str(y + 1, 0, linebuf, (int)strlen(linebuf), linenum_attr);
            Row *r = &E.buf.rows[filerow];
            int avail = cols - linenum_width;
            int start = E.coloff;
            if (start < r->len) {
                int printlen = r->len - start;
                if (printlen > avail) printlen = avail;
                put_str(y + 1, linenum_width, &r->chars[start], printlen, text_attr);
            }
        } else {
            put_cell(y + 1, 2, '~', linenum_attr);
        }
    }

    fill_row(rows - 1, status_attr);
    char left[256];
    snprintf(left, sizeof(left), " line %d/%d  col %d   Ctrl+S save  Ctrl+Z undo  Ctrl+Y redo  Ctrl+X quit",
             E.cy + 1, E.buf.nrows, E.cx + 1);
    put_str(rows - 1, 0, left, (int)strlen(left), status_attr);
    if (E.status_ticks > 0) {
        int slen = (int)strlen(E.status);
        put_str(rows - 1, cols - slen - 2, E.status, slen, status_attr);
        E.status_ticks--;
    }

    COORD size = { (SHORT)cols, (SHORT)rows };
    COORD origin = { 0, 0 };
    SMALL_RECT region = { 0, 0, (SHORT)(cols - 1), (SHORT)(rows - 1) };
    WriteConsoleOutputA(E.hOut, E.screen, size, origin, &region);

    COORD cursor;
    cursor.X = (SHORT)(linenum_width + (E.cx - E.coloff));
    cursor.Y = (SHORT)(E.cy - E.rowoff + 1);
    SetConsoleCursorPosition(E.hOut, cursor);
}

void move_cursor(WORD key) {
    Row *r = &E.buf.rows[E.cy];
    switch (key) {
        case VK_LEFT:
            if (E.cx > 0) E.cx--;
            else if (E.cy > 0) {
                E.cy--;
                E.cx = E.buf.rows[E.cy].len;
            }
            break;
        case VK_RIGHT:
            if (E.cx < r->len) E.cx++;
            else if (E.cy < E.buf.nrows - 1) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case VK_UP:
            if (E.cy > 0) E.cy--;
            break;
        case VK_DOWN:
            if (E.cy < E.buf.nrows - 1) E.cy++;
            break;
        case VK_HOME:
            E.cx = 0;
            break;
        case VK_END:
            E.cx = E.buf.rows[E.cy].len;
            break;
    }
    if (E.cx > E.buf.rows[E.cy].len) E.cx = E.buf.rows[E.cy].len;
    editor_break_run();
}

int confirm_quit_without_save() {
    int rows, cols;
    get_console_size(&rows, &cols);
    const char *msg = " unsaved changes, quit without saving? (y/n)";
    fill_row(rows - 1, BACKGROUND_RED | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    put_str(rows - 1, 0, msg, (int)strlen(msg), BACKGROUND_RED | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    COORD size = { (SHORT)cols, (SHORT)rows };
    COORD origin = { 0, 0 };
    SMALL_RECT region = { 0, 0, (SHORT)(cols - 1), (SHORT)(rows - 1) };
    WriteConsoleOutputA(E.hOut, E.screen, size, origin, &region);

    INPUT_RECORD rec;
    DWORD read;
    while (1) {
        ReadConsoleInputA(E.hIn, &rec, 1, &read);
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            char c = rec.Event.KeyEvent.uChar.AsciiChar;
            if (c == 'y' || c == 'Y') return 1;
            if (c == 'n' || c == 'N') return 0;
        }
    }
}

void prompt_filename() {
    int rows, cols;
    get_console_size(&rows, &cols);
    char name[256] = {0};
    int len = 0;
    while (1) {
        fill_row(rows - 1, BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
        char prompt[300];
        snprintf(prompt, sizeof(prompt), " file name: %s", name);
        put_str(rows - 1, 0, prompt, (int)strlen(prompt), BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
        COORD size = { (SHORT)cols, (SHORT)rows };
        COORD origin = { 0, 0 };
        SMALL_RECT region = { 0, 0, (SHORT)(cols - 1), (SHORT)(rows - 1) };
        WriteConsoleOutputA(E.hOut, E.screen, size, origin, &region);

        INPUT_RECORD rec;
        DWORD read;
        ReadConsoleInputA(E.hIn, &rec, 1, &read);
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
        char c = rec.Event.KeyEvent.uChar.AsciiChar;
        if (vk == VK_RETURN) break;
        if (vk == VK_BACK) {
            if (len > 0) name[--len] = '\0';
            continue;
        }
        if (vk == VK_ESCAPE) { name[0] = '\0'; len = 0; break; }
        if (c >= 32 && c < 127 && len < (int)sizeof(name) - 1) {
            name[len++] = c;
            name[len] = '\0';
        }
    }
    if (len > 0) {
        strncpy(E.filename, name, sizeof(E.filename) - 1);
        ensure_txt_extension(E.filename);
    }
}

int main(int argc, char **argv) {
    memset(&E, 0, sizeof(E));
    buffer_init(&E.buf);
    E.undo.stack = malloc(sizeof(char *) * MAX_UNDO);
    E.redo.stack = malloc(sizeof(char *) * MAX_UNDO);
    E.last_action = ACTION_NONE;

    if (argc >= 2) {
        load_file(argv[1]);
    }

    E.hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    E.hIn = GetStdHandle(STD_INPUT_HANDLE);

    DWORD outMode;
    GetConsoleMode(E.hOut, &outMode);
    SetConsoleMode(E.hOut, outMode & ~ENABLE_WRAP_AT_EOL_OUTPUT);

    DWORD inMode;
    GetConsoleMode(E.hIn, &inMode);
    SetConsoleMode(E.hIn, (inMode | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS) & ~(ENABLE_QUICK_EDIT_MODE | ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));

    CONSOLE_CURSOR_INFO cci;
    GetConsoleCursorInfo(E.hOut, &cci);
    cci.bVisible = TRUE;
    cci.dwSize = 25;
    SetConsoleCursorInfo(E.hOut, &cci);

    set_status("welcome to fishedit");

    int running = 1;
    while (running) {
        draw_screen();

        INPUT_RECORD rec;
        DWORD read;
        ReadConsoleInputA(E.hIn, &rec, 1, &read);

        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) continue;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        KEY_EVENT_RECORD *k = &rec.Event.KeyEvent;
        WORD vk = k->wVirtualKeyCode;
        char c = k->uChar.AsciiChar;
        DWORD ctrl = k->dwControlKeyState;
        int ctrlHeld = (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) ? 1 : 0;
        int altGr = (ctrl & RIGHT_ALT_PRESSED) ? 1 : 0;
        int ctrlOnly = ctrlHeld && !altGr;

        if (ctrlOnly && (vk == 'X' || vk == 'x')) {
            if (E.modified) {
                if (confirm_quit_without_save()) running = 0;
            } else running = 0;
            continue;
        }
        if (ctrlOnly && (vk == 'S' || vk == 's')) {
            if (strlen(E.filename) == 0) prompt_filename();
            save_file();
            continue;
        }
        if (ctrlOnly && (vk == 'Z' || vk == 'z')) {
            do_undo();
            continue;
        }
        if (ctrlOnly && (vk == 'Y' || vk == 'y')) {
            do_redo();
            continue;
        }
        if (ctrlOnly) continue;

        switch (vk) {
            case VK_LEFT:
            case VK_RIGHT:
            case VK_UP:
            case VK_DOWN:
            case VK_HOME:
            case VK_END:
                move_cursor(vk);
                break;
            case VK_PRIOR: {
                int rows, cols;
                get_console_size(&rows, &cols);
                E.cy -= (rows - 2);
                if (E.cy < 0) E.cy = 0;
                if (E.cx > E.buf.rows[E.cy].len) E.cx = E.buf.rows[E.cy].len;
                editor_break_run();
                break;
            }
            case VK_NEXT: {
                int rows, cols;
                get_console_size(&rows, &cols);
                E.cy += (rows - 2);
                if (E.cy >= E.buf.nrows) E.cy = E.buf.nrows - 1;
                if (E.cx > E.buf.rows[E.cy].len) E.cx = E.buf.rows[E.cy].len;
                editor_break_run();
                break;
            }
            case VK_BACK:
                delete_backward();
                break;
            case VK_DELETE:
                delete_forward();
                break;
            case VK_RETURN:
                insert_newline();
                break;
            case VK_TAB:
                for (int i = 0; i < TAB_SIZE; i++) insert_char(' ');
                break;
            default:
                if (c >= 32 && c < 127) {
                    insert_char(c);
                }
                break;
        }
    }

    buffer_free(&E.buf);
    snap_clear(&E.undo);
    snap_clear(&E.redo);
    free(E.undo.stack);
    free(E.redo.stack);
    free(E.screen);
    system("cls");
    return 0;
}