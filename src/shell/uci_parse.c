#include "uci_parse.h"

#include "uci_strings.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Whole-token integer conversion
// ---------------------------------------------------------------------------
//
// Require the WHOLE token to convert. C++'s `is >> value` stops at the first
// non-digit and leaves the rest in the stream, so upstream reads `depth 5abc` as
// depth 5 followed by the junk token `abc`; requiring the whole token turns that
// into a reported bad argument instead. The port source made the same choice,
// and it is what `bad_token` can be defined against at all.

bool uci_parse_u64(const char *token, size_t len, uint64_t *out) {
    if (len == 0)
        return false;

    size_t i = 0;
    if (token[0] == '+')
        i = 1;
    if (i >= len)
        return false;

    uint64_t value = 0;
    for (; i < len; ++i) {
        const char c = token[i];
        if (c < '0' || c > '9')
            return false;
        const uint64_t digit = (uint64_t) (c - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }

    *out = value;
    return true;
}

bool uci_parse_i64(const char *token, size_t len, int64_t *out) {
    if (len == 0)
        return false;

    bool negative = false;
    size_t i = 0;
    if (token[0] == '+' || token[0] == '-') {
        negative = token[0] == '-';
        i = 1;
    }
    if (i >= len)
        return false;

    // Accumulate the magnitude unsigned, so INT64_MIN is representable and no
    // step of the conversion overflows a signed type.
    const uint64_t limit = negative ? (uint64_t) INT64_MAX + 1u : (uint64_t) INT64_MAX;
    uint64_t value = 0;
    for (; i < len; ++i) {
        const char c = token[i];
        if (c < '0' || c > '9')
            return false;
        const uint64_t digit = (uint64_t) (c - '0');
        if (value > (limit - digit) / 10)
            return false;
        value = value * 10 + digit;
    }

    *out = negative ? (int64_t) (~value + 1u) : (int64_t) value;
    return true;
}

bool uci_parse_i32(const char *token, size_t len, int32_t *out) {
    int64_t wide;
    if (!uci_parse_i64(token, len, &wide))
        return false;
    if (wide < INT32_MIN || wide > INT32_MAX)
        return false;
    *out = (int32_t) wide;
    return true;
}

// ---------------------------------------------------------------------------
// go
// ---------------------------------------------------------------------------

static bool take_i64(UciTokens *it, int64_t *dst) {
    const char *token;
    size_t len;
    if (!uci_tokens_next(it, &token, &len))
        return false;  // a missing argument fails exactly as an unparsable one
    return uci_parse_i64(token, len, dst);
}

static bool take_i32(UciTokens *it, int32_t *dst) {
    const char *token;
    size_t len;
    if (!uci_tokens_next(it, &token, &len))
        return false;
    return uci_parse_i32(token, len, dst);
}

static bool take_u64(UciTokens *it, uint64_t *dst) {
    const char *token;
    size_t len;
    if (!uci_tokens_next(it, &token, &len))
        return false;
    return uci_parse_u64(token, len, dst);
}

static void set_bad_token(ParsedLimits *out, const char *keyword) {
    const size_t len = strlen(keyword);
    const size_t n = len < sizeof out->bad_token - 1 ? len : sizeof out->bad_token - 1;
    memcpy(out->bad_token, keyword, n);
    out->bad_token[n] = '\0';
}

void uci_parse_limits(const char *input, size_t len, ParsedLimits *out) {
    *out = (ParsedLimits) { 0 };

    UciBuf searchmoves;
    uci_buf_init(&searchmoves, out->searchmoves, sizeof out->searchmoves);

    UciTokens it;
    uci_tokens_init(&it, input, len);

    const char *token;
    size_t token_len;
    while (uci_tokens_next(&it, &token, &token_len)) {
        if (uci_token_equals(token, token_len, "searchmoves")) {
            // `searchmoves` swallows the rest of the line, so it must be last.
            const char *move;
            size_t move_len;
            while (uci_tokens_next(&it, &move, &move_len)) {
                if (searchmoves.len != 0)
                    uci_buf_append_char(&searchmoves, '\n');
                for (size_t i = 0; i < move_len; ++i)
                    uci_buf_append_char(&searchmoves, uci_ascii_lower(move[i]));
            }
            break;
        } else if (uci_token_equals(token, token_len, "wtime")) {
            if (!take_i64(&it, &out->wtime)) {
                set_bad_token(out, "wtime");
                break;
            }
        } else if (uci_token_equals(token, token_len, "btime")) {
            if (!take_i64(&it, &out->btime)) {
                set_bad_token(out, "btime");
                break;
            }
        } else if (uci_token_equals(token, token_len, "winc")) {
            if (!take_i64(&it, &out->winc)) {
                set_bad_token(out, "winc");
                break;
            }
        } else if (uci_token_equals(token, token_len, "binc")) {
            if (!take_i64(&it, &out->binc)) {
                set_bad_token(out, "binc");
                break;
            }
        } else if (uci_token_equals(token, token_len, "movestogo")) {
            if (!take_i32(&it, &out->movestogo)) {
                set_bad_token(out, "movestogo");
                break;
            }
        } else if (uci_token_equals(token, token_len, "depth")) {
            if (!take_i32(&it, &out->depth)) {
                set_bad_token(out, "depth");
                break;
            }
        } else if (uci_token_equals(token, token_len, "nodes")) {
            if (!take_u64(&it, &out->nodes)) {
                set_bad_token(out, "nodes");
                break;
            }
        } else if (uci_token_equals(token, token_len, "movetime")) {
            if (!take_i64(&it, &out->movetime)) {
                set_bad_token(out, "movetime");
                break;
            }
        } else if (uci_token_equals(token, token_len, "mate")) {
            if (!take_i32(&it, &out->mate)) {
                set_bad_token(out, "mate");
                break;
            }
        } else if (uci_token_equals(token, token_len, "perft")) {
            if (!take_i32(&it, &out->perft)) {
                set_bad_token(out, "perft");
                break;
            }
        } else if (uci_token_equals(token, token_len, "infinite")) {
            out->infinite = 1;
        } else if (uci_token_equals(token, token_len, "ponder")) {
            out->ponder_mode = 1;
        }
        // Ignore any other token, as upstream's if-chain does.
    }

    out->truncated = searchmoves.truncated;
}

// ---------------------------------------------------------------------------
// position
// ---------------------------------------------------------------------------

bool uci_parse_position(const char *input, size_t len, ParsedPosition *out) {
    *out = (ParsedPosition) { 0 };

    UciTokens it;
    uci_tokens_init(&it, input, len);

    const char *token;
    size_t token_len;
    if (!uci_tokens_next(&it, &token, &token_len))
        return false;
    if (uci_token_equals(token, token_len, "position") && !uci_tokens_next(&it, &token, &token_len))
        return false;

    UciBuf fen;
    uci_buf_init(&fen, out->fen, sizeof out->fen);
    UciBuf moves;
    uci_buf_init(&moves, out->moves, sizeof out->moves);

    if (uci_token_equals(token, token_len, "startpos")) {
        uci_buf_append_str(&fen, UCI_START_FEN);
        (void) uci_tokens_next(&it, &token, &token_len);  // consume `moves`, if any
    } else if (uci_token_equals(token, token_len, "fen")) {
        // Rejoin the record: a FEN is six space-separated fields, so it is not
        // one token, and it ends at `moves` or at end of line.
        const char *field;
        size_t field_len;
        while (uci_tokens_next(&it, &field, &field_len)) {
            if (uci_token_equals(field, field_len, "moves"))
                break;
            if (fen.len != 0)
                uci_buf_append_char(&fen, ' ');
            uci_buf_append(&fen, field, field_len);
        }
    } else {
        return false;
    }

    const char *move;
    size_t move_len;
    while (uci_tokens_next(&it, &move, &move_len)) {
        if (moves.len != 0)
            uci_buf_append_char(&moves, '\n');
        uci_buf_append(&moves, move, move_len);
    }

    out->ok = true;
    out->truncated = fen.truncated || moves.truncated;
    return true;
}
