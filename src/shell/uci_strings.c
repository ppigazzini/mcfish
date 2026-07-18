#include "uci_strings.h"

#include <stdio.h>
#include <string.h>

const char *const UciHelpText =
  "\nStockfish is a powerful chess engine for playing and analyzing.\n"
  "It is released as free software licensed under the GNU GPLv3 License.\n"
  "Stockfish is normally used with a graphical user interface (GUI) and implements\n"
  "the Universal Chess Interface (UCI) protocol to communicate with a GUI, an API, etc.\n"
  "For any further information, visit https://github.com/official-stockfish/Stockfish#readme\n"
  "or read the corresponding README.md and Copying.txt files distributed along with this "
  "program.\n";

bool uci_is_space(char byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\v'
        || byte == '\f';
}

char uci_ascii_lower(char byte) {
    return (byte >= 'A' && byte <= 'Z') ? (char) (byte + ('a' - 'A')) : byte;
}

void uci_trim_whitespace(const char *text, size_t len, const char **out, size_t *out_len) {
    size_t start = 0;
    size_t end = len;
    while (start < end && uci_is_space(text[start]))
        ++start;
    while (end > start && uci_is_space(text[end - 1]))
        --end;
    *out = text + start;
    *out_len = end - start;
}

bool uci_token_equals(const char *token, size_t token_len, const char *literal) {
    const size_t literal_len = strlen(literal);
    return token_len == literal_len && memcmp(token, literal, token_len) == 0;
}

// ---------------------------------------------------------------------------
// Iterators
// ---------------------------------------------------------------------------

void uci_tokens_init(UciTokens *it, const char *text, size_t len) {
    it->cur = text;
    it->end = text + len;
}

bool uci_tokens_next(UciTokens *it, const char **token, size_t *token_len) {
    while (it->cur < it->end && uci_is_space(*it->cur))
        ++it->cur;
    if (it->cur >= it->end)
        return false;

    const char *start = it->cur;
    while (it->cur < it->end && !uci_is_space(*it->cur))
        ++it->cur;

    *token = start;
    *token_len = (size_t) (it->cur - start);
    return true;
}

void uci_lines_init(UciLines *it, const char *text, size_t len) {
    it->cur = text;
    it->end = text + len;
    it->done = false;
}

bool uci_lines_next(UciLines *it, const char **line, size_t *line_len) {
    if (it->done)
        return false;

    const char *start = it->cur;
    const char *stop = start;
    while (stop < it->end && *stop != '\n')
        ++stop;

    *line = start;
    *line_len = (size_t) (stop - start);

    if (stop >= it->end)
        it->done = true;  // yield the final field, then stop
    else
        it->cur = stop + 1;
    return true;
}

// ---------------------------------------------------------------------------
// Bounded builder
// ---------------------------------------------------------------------------

void uci_buf_init(UciBuf *b, char *buf, size_t cap) {
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->truncated = cap == 0;
    if (cap != 0)
        buf[0] = '\0';
}

bool uci_buf_append(UciBuf *b, const char *src, size_t len) {
    if (b->truncated)
        return false;
    if (b->len + len + 1 > b->cap) {
        b->truncated = true;
        return false;
    }
    memcpy(b->buf + b->len, src, len);
    b->len += len;
    b->buf[b->len] = '\0';
    return true;
}

bool uci_buf_append_str(UciBuf *b, const char *s) { return uci_buf_append(b, s, strlen(s)); }

bool uci_buf_append_char(UciBuf *b, char c) { return uci_buf_append(b, &c, 1); }

bool uci_buf_vappendf(UciBuf *b, const char *fmt, va_list ap) {
    if (b->truncated)
        return false;

    const size_t room = b->cap - b->len;  // includes the NUL
    const int written = vsnprintf(b->buf + b->len, room, fmt, ap);
    if (written < 0 || (size_t) written + 1 > room) {
        // Leave the buffer as it was: vsnprintf already NUL-terminated at the
        // cut, so rewind to the pre-call length rather than keeping a partial
        // field, which would put a half-rendered number on the wire.
        b->buf[b->len] = '\0';
        b->truncated = true;
        return false;
    }
    b->len += (size_t) written;
    return true;
}

bool uci_buf_appendf(UciBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const bool ok = uci_buf_vappendf(b, fmt, ap);
    va_end(ap);
    return ok;
}
