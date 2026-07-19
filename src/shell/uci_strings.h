// Own the fixed UCI protocol strings and the byte-level primitives the rest of
// the shell tokenizes, trims and builds with.
//
// The invariant is that nothing here allocates and nothing here owns a buffer. A
// token, a line and a trimmed span are all (pointer, length) views into the
// caller's storage and stay valid exactly as long as that storage does; the
// literals are static and outlive the process. UciBuf is the bounded stand-in
// for the growable builders the port source uses — it truncates and records the
// fact rather than growing, so no protocol path can allocate.
//
// Port source: zfish `shell/uci_strings.zig`. Golden: upstream `uci.cpp:87`
// (the whitespace tokenization `std::istringstream >> token` performs) and
// `uci.cpp:594` (UCIEngine::to_lower).

#ifndef MCFISH_UCI_STRINGS_H
#define MCFISH_UCI_STRINGS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

// Hold the position every UCI session starts from. Upstream spells this literal
// in uci.cpp (StartFEN, uci.cpp:41); engine.h spells the same bytes as
// ENGINE_START_FEN for the engine seam. Keep them equal.
#define UCI_START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

// Answer the `help` / `--help` command. Upstream emits this block verbatim
// (uci.cpp:150); it names Stockfish because it describes the protocol and the
// project this is a port of, not the binary's own identity — `id name` is what
// a GUI keys off, and misc.h owns that.
extern const char *const UciHelpText;

// Report whether BYTE is one of the six ASCII whitespace bytes std::isspace
// accepts in the C locale. Upstream's stream extraction splits on exactly these.
bool uci_is_space(char byte);

// Fold one ASCII byte to lower case, leaving every other byte alone. Upstream
// uses std::tolower over unsigned char (uci.cpp:594); restricting it to A-Z is
// the same mapping in the C locale and is locale-independent, which matters
// because a move token must parse identically everywhere.
char uci_ascii_lower(char byte);

// Narrow TEXT to its whitespace-free core. Write the span into *OUT / *OUT_LEN;
// both are views into TEXT. An all-whitespace input yields length 0.
void uci_trim_whitespace(const char *text, size_t len, const char **out, size_t *out_len);

// Compare a (pointer, length) token against a NUL-terminated literal.
bool uci_token_equals(const char *token, size_t token_len, const char *literal);

// ---------------------------------------------------------------------------
// Iterators
// ---------------------------------------------------------------------------

// Split on runs of whitespace, skipping empty fields — the port source's
// `std.mem.tokenizeAny(u8, input, " \t\r\n")`, which is what `is >> token`
// does. Each call yields one non-empty token.
typedef struct {
    const char *cur;
    const char *end;
} UciTokens;

void uci_tokens_init(UciTokens *it, const char *text, size_t len);

// Yield the next token into *TOKEN / *TOKEN_LEN. Return false when exhausted,
// leaving the outputs untouched.
bool uci_tokens_next(UciTokens *it, const char **token, size_t *token_len);

// Split on single '\n' bytes, KEEPING empty fields — the port source's
// `std.mem.splitScalar(u8, input, '\n')`. An empty input yields one empty line,
// and a trailing '\n' yields a final empty line; both matter because the command
// scripts uci_bench builds are '\n'-joined and their consumers skip empty lines
// explicitly rather than relying on the split.
typedef struct {
    const char *cur;
    const char *end;
    bool done;
} UciLines;

void uci_lines_init(UciLines *it, const char *text, size_t len);
bool uci_lines_next(UciLines *it, const char **line, size_t *line_len);

// ---------------------------------------------------------------------------
// Bounded builder
// ---------------------------------------------------------------------------

// Append into fixed storage, keeping the result NUL-terminated at all times.
// Once `truncated` is set it stays set and every later append is a no-op, so a
// caller can build a whole line and test once at the end instead of at every
// step. CAP includes the NUL.
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool truncated;
} UciBuf;

void uci_buf_init(UciBuf *b, char *buf, size_t cap);

// Append LEN bytes / a NUL-terminated string / one byte. Return false once the
// buffer has overflowed.
bool uci_buf_append(UciBuf *b, const char *src, size_t len);
bool uci_buf_append_str(UciBuf *b, const char *s);
bool uci_buf_append_char(UciBuf *b, char c);

// Append a printf-formatted fragment. Return false once the buffer has
// overflowed, or when the fragment itself would not fit.
bool uci_buf_appendf(UciBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool uci_buf_vappendf(UciBuf *b, const char *fmt, va_list ap) __attribute__((format(printf, 2, 0)));

#endif  // MCFISH_UCI_STRINGS_H
