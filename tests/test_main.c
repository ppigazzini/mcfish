// Run the unit and property suite against the engine zone.

#include "../src/engine/board/legality.h"
//
// This links engine/ + platform/ WITHOUT shell/, so it doubles as a zone check:
// a test that needs a shell symbol fails to link, which is the signal that an
// engine file has grown a dependency it should not have.
//
// The property tests are the load-bearing half. Perft is a total check on move
// generation, and the make/unmake round-trip is what catches a do_move that
// forgets to write a StateInfo field — a class of bug that perft alone can miss
// because it restores by popping, not by comparing.

#include "../src/engine/board/attacks.h"
#include "../src/engine/board/bitboard.h"
#include "../src/engine/board/movegen.h"
#include "../src/engine/board/position.h"
#include "../src/engine/board/threats.h"
#include "../src/engine/board/repetition.h"
#include "../src/engine/board/uci_move.h"
#include "../src/engine/eval/evaluate.h"
#include "../src/engine/eval/nnue/nnue_accumulator.h"
#include "../src/engine/eval/nnue/nnue_architecture.h"
#include "../src/engine/eval/nnue/nnue_feature.h"
#include "../src/engine/eval/nnue/nnue_hash.h"
#include "../src/engine/eval/nnue/nnue_parse.h"
#include "../src/engine/eval/nnue/nnue_write.h"
#include "../src/engine/eval/nnue/nnue_weight_storage.h"
#include "../src/engine/search/history.h"
#include "../src/engine/search/movepick.h"
#include "../src/engine/search/search.h"
#include "../src/engine/search/search_common.h"
#include "../src/engine/search/root_pv.h"
#include "../src/engine/search/search_types.h"
#include "../src/engine/search/timeman.h"
#include "../src/engine/search/tt.h"
#include "../src/engine/eval/nnue/simd.h"
#include "../src/platform/numa.h"
#include "../src/platform/syzygy/decode.h"
#include "../src/platform/syzygy/encode.h"
#include "../src/platform/syzygy/wdl.h"
#include "../src/platform/thread_pool.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static int Failures = 0;
static int Checks = 0;

#define CHECK(cond, ...) \
    do { \
        ++Checks; \
        if (!(cond)) { \
            ++Failures; \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static void banner(const char *name) { printf("== %s\n", name); }

// Advance a xorshift64 state. Every randomised test below seeds its own and holds it
// fixed, so a failure names a position or a string that can be reached again.
static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static const char StartFen[] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// ---------------------------------------------------------------- bitboards

static int cmp_u64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *) a, y = *(const uint64_t *) b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void test_bitboards(void) {
    banner("bitboards");

    CHECK(popcount_bb(0) == 0, "popcount(0)");
    CHECK(popcount_bb(~0ULL) == 64, "popcount(full) = %d", popcount_bb(~0ULL));
    CHECK(lsb(square_bb(SQ_H8)) == SQ_H8, "lsb of single bit");
    CHECK(msb(0xFFULL) == 7, "msb of rank 1");

    Bitboard b = square_bb(SQ_A1) | square_bb(SQ_H8);
    CHECK(bb_more_than_one(b), "two bits");
    CHECK(pop_lsb(&b) == SQ_A1, "pop_lsb returns lowest");
    CHECK(!bb_more_than_one(b), "one bit left");

    // Shifts must drop wrapping bits, not rotate them onto the far file.
    CHECK(shift_bb(EAST, square_bb(SQ_H1)) == 0, "east off H-file drops");
    CHECK(shift_bb(WEST, square_bb(SQ_A1)) == 0, "west off A-file drops");
    CHECK(shift_bb(NORTH, square_bb(SQ_A8)) == 0, "north off rank 8 drops");

    // A rook on an empty board reaches 14 squares from anywhere.
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
        CHECK(popcount_bb(attacks_bb(ROOK, s, 0)) == 14, "rook mobility at %d", s);

    // A blocker truncates the ray but stays included (it is capturable).
    const Bitboard occ = square_bb(SQ_A4);
    CHECK(bb_test(attacks_bb(ROOK, SQ_A1, occ), SQ_A4), "blocker included");
    CHECK(!bb_test(attacks_bb(ROOK, SQ_A1, occ), SQ_A5), "beyond blocker excluded");

    CHECK(aligned(SQ_A1, SQ_D4, SQ_H8), "diagonal alignment");
    CHECK(!aligned(SQ_A1, SQ_D4, SQ_H7), "non-alignment");
}

// ---------------------------------------------------------------------- FEN

static void test_fen(void) {
    banner("FEN round-trip and rejection");

    static const char *const valid[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 10",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 11",
        "4k3/8/8/8/8/8/8/4K3 b - - 12 34",
    };

    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; ++i) {
        Position pos;
        StateInfo st;
        char out[128];

        CHECK(pos_set(&pos, valid[i], false, &st), "accept: %s", valid[i]);
        pos_fen(&pos, out);
        CHECK(strcmp(out, valid[i]) == 0, "round-trip:\n    in  %s\n    out %s", valid[i], out);
    }

    // Each of these breaks one specific invariant pos_set must enforce.
    static const char *const invalid[] = {
        "",                                                        // empty
        "8/8/8/8/8/8/8/8 w - - 0 1",                               // no kings
        "4k3/8/8/8/8/8/8/8 w - - 0 1",                             // no white king
        "4k3/8/8/8/8/8/8/4K3 x - - 0 1",                           // bad side to move
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP w KQkq - 0 1",         // too few ranks
        "rnbqkbnr/ppppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1",  // rank overflows
        "4k3/8/8/8/8/8/8/4K2K w - - 0 1",                          // two white kings
    };

    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        Position pos;
        StateInfo st;
        CHECK(!pos_set(&pos, invalid[i], false, &st), "reject: '%s'", invalid[i]);
    }

    // A castling right whose rook or king is missing is DROPPED, not an error.
    // Upstream applies a right only when both squares resolve ("Only apply castling
    // rights if they can be valid", position.cpp) and accepts the position either
    // way. This case previously sat in the reject list above, asserting mcfish's own
    // over-strictness -- verified against the oracle, which renders `w - -` here.
    {
        Position pos;
        StateInfo st;
        char fen[128];
        CHECK(pos_set(&pos, "4k3/8/8/8/8/8/8/4K3 w KQ - 0 1", false, &st),
              "castling rights without rooks must be accepted, not rejected");
        pos_fen(&pos, fen);
        CHECK(strstr(fen, " w - - ") != nullptr, "unbacked castling rights must be dropped: %s",
              fen);
    }

    // The side NOT to move may not be in check: the position could only arise from a
    // move that left its own king en prise (position.cpp:438).
    {
        Position pos;
        StateInfo st;
        CHECK(!pos_set(&pos, "k7/8/8/8/8/8/8/R6K w - - 0 1", false, &st),
              "a capturable enemy king must be rejected");
    }
}

// ------------------------------------------------------------------- perft

typedef struct {
    const char *fen;
    int depth;
    uint64_t nodes;
} PerftCase;

static void test_perft(void) {
    banner("perft (movegen totality)");

    // The standard six. Between them they cover castling both sides, en-passant
    // including the pinned-ep case, under-promotion, and double check.
    static const PerftCase cases[] = {
        { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609 },
        { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -", 4, 4085603 },
        { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -", 6, 11030083 },
        { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq -", 5, 15833292 },
        { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ -", 4, 2103487 },
        { "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - -", 4, 3894594 },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        Position pos;
        StateInfo st;
        CHECK(pos_set(&pos, cases[i].fen, false, &st), "setup %zu", i);

        const uint64_t got = perft(&pos, cases[i].depth, false);
        CHECK(got == cases[i].nodes, "perft(%d) = %llu, want %llu  [%s]", cases[i].depth,
              (unsigned long long) got, (unsigned long long) cases[i].nodes, cases[i].fen);
    }
}

// -------------------------------------------------- make/unmake round-trip

// Walk the legal tree to DEPTH, asserting after every undo that the position is
// byte-identical to what it was before the move. This is what catches a do_move
// that mutates state undo does not restore.
static void walk_roundtrip(Position *pos, int depth) {
    if (depth == 0)
        return;

    ExtMove list[MAX_MOVES];
    const ExtMove *end = generate_legal(pos, list);

    for (const ExtMove *it = list; it != end; ++it) {
        // Snapshot everything undo is responsible for restoring.
        const Key key = pos_key(pos);
        const Color stm = pos->side_to_move;
        Bitboard by_type[PIECE_TYPE_NB], by_color[COLOR_NB];
        Piece board[SQUARE_NB];
        memcpy(by_type, pos->by_type, sizeof by_type);
        memcpy(by_color, pos->by_color, sizeof by_color);
        memcpy(board, pos->board, sizeof board);
        const uint8_t rights = pos->st->castling_rights;
        const Square ep = pos->st->ep_square;

        StateInfo st;
        pos_do_move(pos, it->move, &st, pos_gives_check(pos, it->move), &pos->scratch_dp,
                    &pos->scratch_dts, nullptr);

        // The incrementally-updated key must equal the key of the resulting
        // position computed from scratch — this is the real Zobrist test.
        {
            Position fresh;
            StateInfo fresh_st;
            char fen[128];
            pos_fen(pos, fen);
            if (pos_set(&fresh, fen, false, &fresh_st)) {
                CHECK(pos_key(&fresh) == pos_key(pos),
                      "incremental key != recomputed key after move, fen %s", fen);

                // The auxiliary keys are maintained by a SECOND code path
                // (toggle_aux_keys in do_move) that must agree with the
                // from-scratch classification in compute_key. A king folded into
                // the wrong one is invisible until a history table mis-indexes.
                CHECK(fresh.st->pawn_key == pos->st->pawn_key, "pawn_key drift, fen %s", fen);
                CHECK(fresh.st->minor_piece_key == pos->st->minor_piece_key,
                      "minor_piece_key drift, fen %s", fen);
                CHECK(fresh.st->non_pawn_key[WHITE] == pos->st->non_pawn_key[WHITE],
                      "non_pawn_key[WHITE] drift, fen %s", fen);
                CHECK(fresh.st->non_pawn_key[BLACK] == pos->st->non_pawn_key[BLACK],
                      "non_pawn_key[BLACK] drift, fen %s", fen);
            }
        }

        walk_roundtrip(pos, depth - 1);
        pos_undo_move(pos, it->move);

        CHECK(pos_key(pos) == key, "key not restored");
        CHECK(pos->side_to_move == stm, "side to move not restored");
        CHECK(memcmp(by_type, pos->by_type, sizeof by_type) == 0, "by_type not restored");
        CHECK(memcmp(by_color, pos->by_color, sizeof by_color) == 0, "by_color not restored");
        CHECK(memcmp(board, pos->board, sizeof board) == 0, "board not restored");
        CHECK(pos->st->castling_rights == rights, "castling rights not restored");
        CHECK(pos->st->ep_square == ep, "ep square not restored");

        if (Failures > 20)
            return;  // stop the flood; the first few are the diagnostic
    }
}

static void test_roundtrip(void) {
    banner("make/unmake round-trip and incremental Zobrist");

    static const char *const fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -",
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq -",
    };

    for (size_t i = 0; i < sizeof fens / sizeof fens[0]; ++i) {
        Position pos;
        StateInfo st;
        CHECK(pos_set(&pos, fens[i], false, &st), "setup %zu", i);
        walk_roundtrip(&pos, 3);
    }
}

// Hold the parser against strings nobody wrote down.
//
// test_fen's accept and reject tables are curated: every entry is a string someone
// thought of, and the reject list can only hold invariants someone already knew to
// break. The class it cannot reach is the string that is ACCEPTED and should not have
// been -- there is no fixture for a bug nobody anticipated. `tools/uci_fuzz.py` drives
// arbitrary text at the shipped binary, but it is a nightly subprocess lane and its
// only verdict is that the process did not die: a malformed board the parser accepted
// passes it, because nothing downstream examines what came back.
//
// Taken from ../rfish, whose engine-side harness asserts the parser must reject or
// accept and never panic, and that a position it did accept is coherent enough to
// generate moves from. In C "never panic" is what ASan+UBSan answer, which this gate
// already runs under; the coherence half is the assertion.
//
// Mutating valid FENs, not only drawing random strings: a mutant of a legal position
// lands in the almost-valid neighbourhood where a parser actually dies, and it is the
// only way to get an accept rate high enough for the checks below to run at all --
// which is why the accepted count is floored rather than assumed.
static void test_fen_parse_robustness(void) {
    banner("FEN parse robustness");

    static const char *const bases[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 10",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 11",
        "4k3/8/8/8/8/8/8/4K3 b - - 12 34",
    };
    // Chess-shaped, so a substitution lands near a FEN rather than in random bytes.
    static const char alphabet[] = "rnbqkpRNBQKP12345678/ -abcdefghwKQkq0123456789";

    enum { CASES = 4000, BUF = 160 };
    uint64_t rng = 0x9E3779B97F4A7C15ULL;
    int accepted = 0;

    for (int i = 0; i < CASES; ++i) {
        char text[BUF];
        size_t len;

        if (i % 8 == 0) {
            // A share drawn from nothing at all: a parser that indexes before it
            // validates does not need a plausible prefix to walk off the end.
            len = 1 + xorshift64(&rng) % 90;
            for (size_t j = 0; j < len; ++j)
                text[j] = alphabet[xorshift64(&rng) % (sizeof alphabet - 1)];
        } else {
            const char *const base = bases[xorshift64(&rng) % (sizeof bases / sizeof bases[0])];
            len = strlen(base);
            memcpy(text, base, len);
            const int edits = 1 + (int) (xorshift64(&rng) % 3);
            for (int e = 0; e < edits && len > 0; ++e) {
                const uint64_t what = xorshift64(&rng) % 3;
                const size_t at = xorshift64(&rng) % len;
                if (what == 0) {  // substitute
                    text[at] = alphabet[xorshift64(&rng) % (sizeof alphabet - 1)];
                } else if (what == 1) {  // delete
                    memmove(text + at, text + at + 1, len - at - 1);
                    --len;
                } else if (len + 1 < BUF) {  // insert
                    memmove(text + at + 1, text + at, len - at);
                    text[at] = alphabet[xorshift64(&rng) % (sizeof alphabet - 1)];
                    ++len;
                }
            }
            // Truncation is its own class: a parser that trusts a field to be there
            // reads past the end rather than seeing a wrong character.
            if (xorshift64(&rng) % 4 == 0)
                len = xorshift64(&rng) % (len + 1);
        }
        text[len] = '\0';

        for (int chess960 = 0; chess960 <= 1; ++chess960) {
            Position pos;
            StateInfo st;
            if (!pos_set(&pos, text, chess960 != 0, &st))
                continue;  // rejecting is a valid answer; nothing here says which

            // Accepted. Then it must BE a position: move generation must run over it,
            // and what it renders of itself must parse back to the same board.
            ++accepted;
            ExtMove list[MAX_MOVES];
            const ExtMove *const end = generate_legal(&pos, list);
            CHECK(end >= list && end - list <= MAX_MOVES, "move list stays in bounds [%s]", text);

            char fen[BUF];
            pos_fen(&pos, fen);
            Position back;
            StateInfo back_st;
            if (!pos_set(&back, fen, chess960 != 0, &back_st)) {
                CHECK(false, "an accepted position renders a FEN it rejects: '%s' -> '%s'", text,
                      fen);
                continue;
            }
            CHECK(pos_key(&back) == pos_key(&pos), "round-trip changes the key: '%s' -> '%s'", text,
                  fen);
            CHECK(memcmp(back.board, pos.board, sizeof back.board) == 0,
                  "round-trip changes the board: '%s' -> '%s'", text, fen);

            if (Failures > 20)
                return;  // stop the flood; the first few are the diagnostic
        }
    }

    // A run that accepted nothing would report every check above as passed while
    // having made none of them. The floor says the mutation rate still lands inside
    // the language; set it well under what the walk actually accepts, so a new base
    // FEN never trips it and a collapse always does. The count travels in the failure
    // message rather than in a comment that cannot be recomputed.
    CHECK(accepted >= 100, "the mutations still produce positions the parser accepts, got %d",
          accepted);
}

// Unwind a LONG line, where a shallow exhaustive tree cannot go.
//
// test_roundtrip above is total and therefore shallow: every legal move to ply 3 from
// four fixtures, with restoration asserted after each undo. Three plies is before the
// states that only exist deep in a line -- the fifty-move counter part way to 100, a
// castling right that expired twenty plies back, an en passant square offered and
// declined, a repetition in the chain. And a per-ply check cannot see a key that
// DESYNCS and RESYNCS: only bringing the whole line back can.
//
// Taken from ../rfish, whose engine-side walk keeps the played line and compares the
// board after undoing all of it -- the shape it records a fifty-move mixing bug
// having had. mcfish's own gates are blind the same way: perft restores by popping
// rather than comparing, and the fuzz walk never undoes at all.
static void test_deep_line_roundtrip(void) {
    banner("deep-line make/unmake round-trip");

    enum { LINES = 200, MAX_PLIES = 60 };
    uint64_t rng = 0x2545F4914F6CDD1DULL;

    for (int line = 0; line < LINES; ++line) {
        Position pos;
        StateInfo root_st;
        StateInfo chain[MAX_PLIES];
        Move played[MAX_PLIES];

        CHECK(pos_set(&pos, StartFen, false, &root_st), "start position");

        // Snapshot everything the unwind has to give back.
        const Key key0 = pos_key(&pos);
        const Color stm0 = pos.side_to_move;
        Bitboard by_type0[PIECE_TYPE_NB], by_color0[COLOR_NB];
        Piece board0[SQUARE_NB];
        memcpy(by_type0, pos.by_type, sizeof by_type0);
        memcpy(by_color0, pos.by_color, sizeof by_color0);
        memcpy(board0, pos.board, sizeof board0);
        const uint8_t rights0 = pos.st->castling_rights;
        const Square ep0 = pos.st->ep_square;
        const int rule50_0 = pos.st->rule50;

        int plies = 0;
        for (; plies < MAX_PLIES; ++plies) {
            ExtMove list[MAX_MOVES];
            const ExtMove *const end = generate_legal(&pos, list);
            const ptrdiff_t count = end - list;
            if (count == 0)
                break;  // checkmate or stalemate ends the line early

            const Move m = list[xorshift64(&rng) % (uint64_t) count].move;
            played[plies] = m;
            pos_do_move(&pos, m, &chain[plies], pos_gives_check(&pos, m), &pos.scratch_dp,
                        &pos.scratch_dts, nullptr);

            // The incremental keys must equal the from-scratch keys of the same board
            // AT EVERY DEPTH. test_roundtrip proves this to ply 3, where no right has
            // expired and the halfmove clock is still near zero.
            Position fresh;
            StateInfo fresh_st;
            char fen[128];
            pos_fen(&pos, fen);
            if (!pos_set(&fresh, fen, false, &fresh_st)) {
                CHECK(false, "a position the engine reached does not parse back: %s", fen);
                continue;
            }
            CHECK(pos_key(&fresh) == pos_key(&pos), "key drift at ply %d, fen %s", plies, fen);
            CHECK(fresh.st->pawn_key == pos.st->pawn_key, "pawn_key drift, fen %s", fen);
            CHECK(fresh.st->minor_piece_key == pos.st->minor_piece_key,
                  "minor_piece_key drift, fen %s", fen);
            CHECK(fresh.st->material_key == pos.st->material_key, "material_key drift, fen %s",
                  fen);
            CHECK(fresh.st->non_pawn_key[WHITE] == pos.st->non_pawn_key[WHITE],
                  "non_pawn_key[WHITE] drift, fen %s", fen);
            CHECK(fresh.st->non_pawn_key[BLACK] == pos.st->non_pawn_key[BLACK],
                  "non_pawn_key[BLACK] drift, fen %s", fen);
        }

        while (plies > 0)
            pos_undo_move(&pos, played[--plies]);

        CHECK(pos.st == &root_st, "the state chain unwinds to the root it started from");
        CHECK(pos_key(&pos) == key0, "the key survives unwinding the whole line");
        CHECK(pos.side_to_move == stm0, "side to move restored");
        CHECK(memcmp(by_type0, pos.by_type, sizeof by_type0) == 0, "by_type restored");
        CHECK(memcmp(by_color0, pos.by_color, sizeof by_color0) == 0, "by_color restored");
        CHECK(memcmp(board0, pos.board, sizeof board0) == 0, "board restored");
        CHECK(pos.st->castling_rights == rights0, "castling rights restored");
        CHECK(pos.st->ep_square == ep0, "ep square restored");
        CHECK(pos.st->rule50 == rule50_0, "halfmove clock restored");

        if (Failures > 20)
            break;  // stop the flood; the first few are the diagnostic
    }
}

static void test_null_move(void) {
    banner("null move round-trip");

    Position pos;
    StateInfo st;
    CHECK(
      pos_set(&pos, "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -", false, &st),
      "null-move setup");

    const Key key = pos_key(&pos);
    const Color stm = pos.side_to_move;

    StateInfo null_st;
    pos_do_null_move(&pos, &null_st, &pos.scratch_dp, &pos.scratch_dts);
    CHECK(pos.side_to_move != stm, "null move flips the side");
    CHECK(pos_key(&pos) != key, "null move changes the key");

    pos_undo_null_move(&pos);
    CHECK(pos_key(&pos) == key, "null move key restored");
    CHECK(pos.side_to_move == stm, "null move side restored");
}

// -------------------------------------------------------- legality/movegen

static void test_legality(void) {
    banner("legality");

    Position pos;
    StateInfo st;

    // A knight pinned along the e-file has no legal move; only the king's remain.
    // The black king sits on a8 so the pinning rook on e8 is the only e-file piece.
    CHECK(pos_set(&pos, "k3r3/8/8/8/8/8/4N3/4K3 w - -", false, &st), "knight-check setup");
    CHECK(checkers(&pos) == 0, "pin fixture is not itself a check");
    ExtMove list[MAX_MOVES];
    int count = (int) (generate_legal(&pos, list) - list);
    for (int i = 0; i < count; ++i)
        CHECK(type_of_piece(piece_on(&pos, move_from(list[i].move))) == KING,
              "pinned knight generated a move");

    // Double check: only king moves are legal. Nf3 and Rh1 both bear on e1.
    // ASSERT the precondition rather than gate on it -- the previous fixture here
    // was a single check (a knight on c3 does not attack e1), so the loop below
    // never ran and double check went untested while the suite reported a pass.
    CHECK(pos_set(&pos, "k7/8/8/8/8/5n2/8/4K2r w - -", false, &st), "pinned-piece setup");
    CHECK(bb_more_than_one(checkers(&pos)), "double-check fixture is not a double check");
    count = (int) (generate_legal(&pos, list) - list);
    CHECK(count > 0, "double-check fixture has no legal move -- it is mate, not a test");
    for (int i = 0; i < count; ++i)
        CHECK(type_of_piece(piece_on(&pos, move_from(list[i].move))) == KING,
              "double check allowed a non-king move");

    // Every generated legal move must survive pos_legal, and the legal set must
    // be exactly the pseudo-legal set filtered by it.
    static const char *const fens[] = {
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ -",
    };
    for (size_t i = 0; i < sizeof fens / sizeof fens[0]; ++i) {
        CHECK(pos_set(&pos, fens[i], false, &st), "setup %zu", i);

        ExtMove pseudo[MAX_MOVES];
        const int pn =
          (int) (generate(&pos, pseudo, checkers(&pos) ? GEN_EVASIONS : GEN_NON_EVASIONS) - pseudo);
        int legal_from_pseudo = 0;
        for (int j = 0; j < pn; ++j)
            legal_from_pseudo += pos_legal(&pos, pseudo[j].move);

        count = (int) (generate_legal(&pos, list) - list);
        CHECK(count == legal_from_pseudo, "legal set size %d != filtered pseudo %d [%s]", count,
              legal_from_pseudo, fens[i]);

        // Captures + quiets must partition the non-evasion set exactly.
        if (!checkers(&pos)) {
            ExtMove caps[MAX_MOVES], quiets[MAX_MOVES];
            const int cn = (int) (generate(&pos, caps, GEN_CAPTURES) - caps);
            const int qn = (int) (generate(&pos, quiets, GEN_QUIETS) - quiets);
            CHECK(cn + qn == pn, "captures %d + quiets %d != all %d [%s]", cn, qn, pn, fens[i]);
        }
    }
}

static void test_uci_move_strings(void) {
    banner("UCI move encoding");

    Position pos;
    StateInfo st;
    CHECK(pos_set(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", false, &st),
          "startpos setup");

    const Move e2e4 = move_from_uci(&pos, "e2e4");
    CHECK(e2e4 != MOVE_NONE, "e2e4 parses");
    CHECK(move_from(e2e4) == 12 && move_to(e2e4) == 28, "e2e4 squares");

    char buf[8];
    move_to_uci(&pos, e2e4, buf);
    CHECK(strcmp(buf, "e2e4") == 0, "e2e4 round-trip, got %s", buf);

    CHECK(move_from_uci(&pos, "e2e5") == MOVE_NONE, "illegal move rejected");
    CHECK(move_from_uci(&pos, "zzzz") == MOVE_NONE, "garbage rejected");

    // Castling must print as the KING's destination in standard chess, even though
    // it is stored as king-captures-rook.
    CHECK(pos_set(&pos, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq -", false, &st), "castling setup");
    const Move oo = move_from_uci(&pos, "e1g1");
    CHECK(oo != MOVE_NONE && move_type(oo) == CASTLING, "e1g1 is castling");
    move_to_uci(&pos, oo, buf);
    CHECK(strcmp(buf, "e1g1") == 0, "castling prints king destination, got %s", buf);

    // Under-promotion must be distinguishable from queening.
    CHECK(pos_set(&pos, "8/P6k/8/8/8/8/8/K7 w - -", false, &st), "promotion setup");
    const Move promo_q = move_from_uci(&pos, "a7a8q");
    const Move promo_n = move_from_uci(&pos, "a7a8n");
    CHECK(promo_q != MOVE_NONE && move_promotion(promo_q) == QUEEN, "a7a8q");
    CHECK(promo_n != MOVE_NONE && move_promotion(promo_n) == KNIGHT, "a7a8n");
    CHECK(promo_q != promo_n, "promotions encode distinctly");
}

// ------------------------------------------------------------------- eval

static void test_evaluate(void) {
    banner("evaluation");

    Position pos;
    StateInfo st;

    // The start position is symmetric, so the score is the tempo bonus alone.
    CHECK(pos_set(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", false, &st),
          "startpos setup");
    const Value start = evaluate(&pos);
    CHECK(start > 0 && start < 100, "start eval is a small tempo bonus, got %d", start);

    // A mirrored position must evaluate identically for the side to move, or the
    // eval has a color bias the search would happily exploit into nonsense.
    CHECK(pos_set(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1", false, &st),
          "startpos setup (black)");
    CHECK(evaluate(&pos) == start, "eval is color-symmetric");

    // A queen up must read as clearly winning.
    CHECK(pos_set(&pos, "4k3/8/8/8/8/8/8/3QK3 w - -", false, &st), "white-queen setup");
    CHECK(evaluate(&pos) > QUEEN_VALUE / 2, "queen up is winning, got %d", evaluate(&pos));

    CHECK(pos_set(&pos, "3qk3/8/8/8/8/8/8/4K3 w - -", false, &st), "black-queen setup");
    CHECK(evaluate(&pos) < -QUEEN_VALUE / 2, "queen down is losing, got %d", evaluate(&pos));
}

// ------------------------------------------------------------------ search

static void test_search(void) {
    banner("search");

    CHECK(tt_resize(8), "TT allocates");

    Position pos;
    StateInfo st;
    SearchLimits limits = { .depth = 6 };

    // Mate in one must be found and reported as mate 1, not as a large cp score.
    CHECK(pos_set(&pos, "7k/6pp/8/8/8/8/8/R6K w - -", false, &st), "rook-endgame setup");
    SearchResult r = search_go(&pos, &limits);
    CHECK(r.score >= VALUE_MATE_IN_MAX_PLY, "mate score, got %d", r.score);
    CHECK(r.score == mate_in(1), "mate in 1, got %d", r.score);

    char buf[8];
    move_to_uci(&pos, r.best_move, buf);
    CHECK(strcmp(buf, "a1a8") == 0, "finds Ra8#, got %s", buf);

    // A checkmated position has no move and a mated score at ply 0.
    CHECK(pos_set(&pos, "7k/6Q1/6K1/8/8/8/8/8 b - -", false, &st), "mate-in-one setup");
    r = search_go(&pos, &limits);
    CHECK(r.best_move == MOVE_NONE, "no move when mated");
    CHECK(r.score == mated_in(0), "mated_in(0), got %d", r.score);

    // Stalemate is a draw, not a loss.
    CHECK(pos_set(&pos, "7k/5Q2/8/8/8/8/8/6K1 b - -", false, &st), "stalemate setup");
    r = search_go(&pos, &limits);
    CHECK(r.score == VALUE_DRAW, "stalemate is a draw, got %d", r.score);

    // Free material must be taken.
    CHECK(pos_set(&pos, "4k3/8/8/3q4/4B3/8/8/4K3 w - -", false, &st), "hanging-queen setup");
    r = search_go(&pos, &limits);
    move_to_uci(&pos, r.best_move, buf);
    CHECK(strcmp(buf, "e4d5") == 0, "captures the hanging queen, got %s", buf);

    // The search must be deterministic: same position, same TT state, same nodes.
    CHECK(
      pos_set(&pos, "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -", false, &st),
      "kiwipete setup");
    // Clear the SEARCH as well as the table. History and the per-game scalars
    // outlive a `go` by design (upstream resets them only on ucinewgame), so two
    // runs are only comparable from the same game-start state. Clearing just the
    // table here would assert that history does NOT carry -- the opposite of what
    // upstream does, and the reason bench diverged.
    tt_clear();
    search_clear();
    const uint64_t first = search_go(&pos, &limits).nodes;
    tt_clear();
    search_clear();
    const uint64_t second = search_go(&pos, &limits).nodes;
    CHECK(first == second, "search is deterministic: %llu vs %llu", (unsigned long long) first,
          (unsigned long long) second);

    // Search must stay correct with the table effectively disabled — nothing may
    // depend on a probe hitting.
    tt_clear();
    CHECK(pos_set(&pos, "7k/6pp/8/8/8/8/8/R6K w - -", false, &st), "rook-endgame setup");
    r = search_go(&pos, &limits);
    CHECK(r.score == mate_in(1), "mate found with a cleared table, got %d", r.score);

    tt_free();
}

// Drive the real search from positions no fixture names, with the network loaded.
//
// Two gaps meet here, and both are in the ALWAYS-RUN suite rather than in the search
// itself. Every search in test_search runs from a hand-picked FEN; and every one of
// them runs before the first eval_nnue_load, so the suite's searches all evaluate
// through the classical fallback and the accumulator's push/pop is never entered
// from inside a search. tools/fuzz_search.c is exactly this walk under libFuzzer,
// but `fuzz-search` is a nightly lane deliberately out of `parity`, so between
// nightlies nothing under ASan+UBSan enters the node body from an arbitrary board.
//
// Taken from ../zfish, which moved the same class out of its fuzz target into its
// always-run gate after a root-setup bug that only bit non-startpos roots. That
// specific defect has no analogue here -- zfish's headless entry copies a live board
// and must cut the root's `previous` chain itself, where `search_go` takes the
// caller's chain as it stands -- but the class it belongs to does.
//
// Run it without a net rather than skipping: movegen, the picker, the TT, pruning
// and qsearch are all still reached, and only the accumulator half goes uncovered.
static void test_search_reached_positions(void) {
    banner("search from reached positions");

    const bool net = eval_nnue_init() && eval_nnue_load("resources/", nullptr);
    if (!net)
        printf("  NOTE: resources/%s not present; the walk runs on the classical eval\n",
               eval_nnue_default_file_name());

    CHECK(tt_resize(8), "TT allocates");
    tt_clear();
    search_clear();
#ifdef MCFISH_ACC_STATS
    nnue_acc_stats_reset();
#endif

    // 24 plies is deep enough to leave book shapes and reach unbalanced middlegames;
    // depth 1..4 keeps 200 searches inside a few seconds under ASan+UBSan. Neither is
    // a coverage claim -- a bug in the node body is as reachable at depth 3 as at 8,
    // which is the same reasoning tools/fuzz_search.c's caps carry.
    enum { WALK_PLIES = 24, ITERATIONS = 200 };
    uint64_t rng = 0x1D8E4C55A5F13B7BULL;

    for (int iteration = 0; iteration < ITERATIONS; ++iteration) {
        Position pos;
        StateInfo root_st;
        StateInfo walk_st[WALK_PLIES];

        CHECK(pos_set(&pos, StartFen, false, &root_st), "start position");

        const int line = (int) (xorshift64(&rng) % (WALK_PLIES + 1));
        for (int ply = 0; ply < line; ++ply) {
            ExtMove walk[MAX_MOVES];
            const ExtMove *const walk_end = generate_legal(&pos, walk);
            const ptrdiff_t walk_count = walk_end - walk;
            if (walk_count == 0)
                break;  // checkmate or stalemate: nowhere left to walk

            const Move m = walk[xorshift64(&rng) % (uint64_t) walk_count].move;
            pos_do_move(&pos, m, &walk_st[ply], pos_gives_check(&pos, m), &pos.scratch_dp,
                        &pos.scratch_dts, nullptr);
        }

        ExtMove list[MAX_MOVES];
        const ExtMove *const end = generate_legal(&pos, list);
        const ptrdiff_t count = end - list;

        // Snapshot before the search: the FEN names the board in any failure report,
        // and the key is what says the search left the caller's root as it found it.
        char fen[128];
        pos_fen(&pos, fen);
        const Key key = pos_key(&pos);

        const SearchLimits limits = { .depth = 1 + (int) (xorshift64(&rng) % 4) };
        const SearchResult r = search_go(&pos, &limits);

        CHECK(pos_key(&pos) == key, "the search leaves the caller's root untouched [%s]", fen);

        if (count == 0) {
            CHECK(r.best_move == MOVE_NONE, "no move at a terminal root [%s]", fen);
        } else {
            bool generated = false;
            for (const ExtMove *it = list; it != end; ++it)
                generated = generated || it->move == r.best_move;
            CHECK(generated, "the best move is one the root generates [%s]", fen);
            CHECK(r.score > -VALUE_INFINITE && r.score < VALUE_INFINITE,
                  "score in range, got %d [%s]", r.score, fen);
            CHECK(r.nodes > 0, "the search visited nodes [%s]", fen);
            CHECK(r.depth_reached >= limits.depth, "reached the requested depth, got %d [%s]",
                  r.depth_reached, fen);
        }

        if (Failures > 20)
            break;  // stop the flood; the first few are the diagnostic
    }

#ifdef MCFISH_ACC_STATS
    // Assert the half this test exists for actually ran. Loading the net is not the
    // same as reaching it: an evaluate() that fell back would leave every check above
    // green and the accumulator untouched, which is precisely the shape
    // test_nnue_accumulator_paths documents as invisible to a value gate.
    if (net) {
        const NnueAccStats *const s = nnue_acc_stats();
        CHECK(s->shared_walk > 0 || s->split_walk > 0,
              "the search entered the accumulator (%llu shared, %llu split walks)",
              (unsigned long long) s->shared_walk, (unsigned long long) s->split_walk);
    }
#endif

    tt_free();
}

static void test_tt(void) {
    banner("transposition table");

    CHECK(tt_resize(1), "resize");
    tt_clear();

    const Key key = 0x0123456789ABCDEFULL;
    TTProbeResult r = tt_probe(key);
    CHECK(!r.found, "empty table misses");

    tt_save(r.writer, key, 42, true, BOUND_EXACT, 7, 1234, 40);
    r = tt_probe(key);
    CHECK(r.found, "stored entry is found");
    CHECK(r.data.value == 42, "value survives, got %d", r.data.value);
    CHECK(r.data.move == 1234, "move survives");
    CHECK(r.data.bound == BOUND_EXACT, "bound survives");
    CHECK(r.data.depth == 7, "depth survives the entry offset, got %d", r.data.depth);
    CHECK(r.data.is_pv, "the is-PV bit round-trips");

    // The is-PV bit must not bleed into the bound or the generation.
    tt_save(r.writer, key, 42, false, BOUND_LOWER, 7, 1234, 40);
    r = tt_probe(key);
    CHECK(!r.data.is_pv && r.data.bound == BOUND_LOWER, "pv and bound stay separate");

    tt_clear();
    r = tt_probe(key);
    CHECK(!r.found, "clear empties the table");

    // Mate scores are stored root-relative and must survive the re-basing.
    for (int ply = 0; ply < 10; ++ply) {
        const Value v = mate_in(ply + 3);
        CHECK(value_from_tt(value_to_tt(v, ply), ply) == v, "mate re-base at ply %d", ply);
    }
    CHECK(value_from_tt(value_to_tt(100, 5), 5) == 100, "cp score unaffected");

    tt_free();
}

static void test_draw_detection(void) {
    banner("draw detection");

    Position pos;
    StateInfo st;
    StateInfo chain[16];

    // Shuffle the kings back and forth: the position repeats and must be seen.
    CHECK(pos_set(&pos, "4k3/8/8/8/8/8/8/4K3 w - -", false, &st), "bare-kings setup");
    const Move moves[] = {
        move_from_uci(&pos, "e1e2"),
    };
    CHECK(moves[0] != MOVE_NONE, "e1e2 legal");

    // A bare-kings shuffle gives no check on any move, so the gives_check
    // argument pos_do_move now trusts is a constant false here.
    static const char *const shuffle[] = { "e1e2", "e8e7", "e2e1", "e7e8",
                                           "e1e2", "e8e7", "e2e1", "e7e8" };
    int n = 0;
    for (size_t i = 0; i < sizeof shuffle / sizeof shuffle[0]; ++i)
        pos_do_move(&pos, move_from_uci(&pos, shuffle[i]), &chain[n++], false, &pos.scratch_dp,
                    &pos.scratch_dts, nullptr);

    CHECK(pos_is_draw(&pos, 8), "threefold repetition detected");

    // The 50-move rule fires on the halfmove clock alone.
    CHECK(pos_set(&pos, "4k3/8/8/8/8/8/8/4K3 w - - 100 60", false, &st), "setup: clock at 100");
    CHECK(pos_is_draw(&pos, 0), "50-move rule detected");

    CHECK(pos_set(&pos, "4k3/8/8/8/8/8/8/4K3 w - - 99 60", false, &st), "setup: clock at 99");
    CHECK(!pos_is_draw(&pos, 0), "99 plies is not yet a draw");
}

// nnue_dot_step is the ONE reducing primitive in simd.h, and the only place the
// NNUE kernels depend on something the C standard does not give them: on x86 it is
// pmaddubsw + pmaddwd, and pmaddubsw SATURATES its int16 intermediate. simd.h argues
// that saturation is unreachable because affine inputs are activation outputs capped
// at 127 and weights are int8, so the largest pair sum is 127*128*2 = 32512. That is
// an argument, not a check -- and if it is ever wrong the engine does not crash, it
// searches a different tree while every other gate stays green. So drive the
// primitive against an independent scalar reference here, hardest at the boundary
// the argument turns on.
//
// The shape of the gate is one source, two lowerings, and a check that they agree.
// A bug of exactly this shape -- a vector construct correct only under one
// compiler's chosen representation -- benches a wrong number while every other
// gate stays green, because nothing else here runs the scalar path at all.
static void test_nnue_dot4(void) {
    // A reference that shares nothing with simd.h: plain scalar C, no vector type,
    // no intrinsic, and deliberately int32 throughout so a saturating intermediate
    // in the implementation shows up as a disagreement rather than being mirrored.
    uint8_t in[4];
    // 16-byte aligned per nnue_affine_32's weight contract: the SSSE3 tier of
    // nnue_dot_step loads weight rows with an aligned, foldable load.
    alignas(16) int8_t w[NNUE_DOT_LANES * 4];
    uint64_t rng = 0x9E3779B97F4A7C15ULL;

    for (int trial = 0; trial < 20000; ++trial) {
        for (int i = 0; i < NNUE_DOT_LANES * 4; ++i) {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            if (trial < 200) {
                // The boundary the saturation argument rests on: max magnitude in
                // every lane, both signs of weight. If pmaddubsw ever saturates on
                // legal data, it saturates here first.
                if (i < 4)
                    in[i] = 127;
                w[i] = (int8_t) ((rng & 1) ? -128 : 127);
            } else {
                if (i < 4)
                    in[i] = (uint8_t) (rng % 128);  // activations are capped at 127
                w[i] = (int8_t) ((int) (rng >> 8 & 0xFF) - 128);
            }
        }

        // The input group is FOUR bytes broadcast across the step's width, so every
        // output lane sees the same four -- only the weights advance.
        int32_t expect[NNUE_DOT_LANES];
        for (int q = 0; q < NNUE_DOT_LANES; ++q) {
            int32_t acc = 0;
            for (int s = 0; s < 4; ++s)
                acc += (int32_t) in[s] * (int32_t) w[q * 4 + s];
            expect[q] = acc;
        }

        uint32_t packed;
        memcpy(&packed, in, sizeof packed);
        const NnueDotAcc got = nnue_dot_step(nnue_dot_zero(), packed, w);
        for (int q = 0; q < NNUE_DOT_LANES; ++q)
            CHECK(nnue_dot_lane(got, (size_t) q) == expect[q],
                  "nnue_dot_step disagrees with the scalar reference");
    }

    // State the bound the whole argument turns on, so a future net format or a wider
    // activation cap trips a test rather than silently saturating.
    CHECK(127 * 128 * 2 < 32768, "pmaddubsw pair sum must not reach int16 saturation");
}

// --------------------------------------------- construction-path 0xAA poison
//
// The picker constructor and the net parse both leave memory deliberately
// unwritten on writes-before-reads arguments: init_common leaves the five
// cursor/span fields and the cont_hist pointers unset (movepick.c), and the
// weight arena is handed out uninitialised because a successful parse writes
// every byte a reader can reach (nnue_weight_storage.c). Those arguments are
// correct today and invisible to every other gate the day they regress: a
// fresh large allocation arrives kernel-zeroed, so a read of a byte nothing
// wrote reads a plausible 0 instead of crashing. Both tests below run the
// construction path over memory pre-filled with 0xAA, so a regressed implicit
// zero-dependency fails here rather than in play. (The one allocator whose
// zero-fill IS the contract is page_alloc -- the TT skip-clear and the
// SearchWorker block state that dependence where they rely on it; do not
// poison those.)

// Fill the picker with PATTERN, construct it the way the search does, and
// drain it. SKIP_AFTER >= 0 calls movepick_skip_quiets after that many moves,
// exercising the skip-quiets stage walk. Return the move count, or CAP + 1
// when the picker ran away (a poisoned cursor that survives construction can
// present as an endless span, not just a wrong move).
static size_t drain_picker(uint8_t pattern,
                           const Position *pos,
                           int depth,
                           bool probcut,
                           long skip_after,
                           Move *out,
                           size_t cap) {
    Histories *const h = histories();
    if (h == nullptr)
        return cap + 1;

    // Give (ss-1)..(ss-6) the sentinel continuation page, as the search does
    // for a frame with no move behind it (search_set_cont_hist).
    Stack frames[8] = { 0 };
    SharedStat *const page = cont_hist_page(h, false, false, NO_PIECE, SQ_A1);
    for (size_t i = 0; i < 8; ++i)
        frames[i].continuation_history = page;
    Stack *const ss = &frames[7];

    // The picker scores through the CALLER's continuation array, as upstream's
    // constructor takes it, so the array has to outlive the picker -- declare it
    // in the same scope, which is what the node body does.
    const SharedStat *const cont_hist[6] = {
        (ss - 1)->continuation_history, (ss - 2)->continuation_history,
        (ss - 3)->continuation_history, (ss - 4)->continuation_history,
        (ss - 5)->continuation_history, (ss - 6)->continuation_history,
    };

    MovePicker mp;
    memset(&mp, pattern, sizeof mp);

    if (probcut) {
        movepick_init_probcut(&mp, pos, h, MOVE_NONE, 1);
        mp.stage = MP_PROBCUT_TT + 1;  // no usable TT move
    } else {
        movepick_init(&mp, pos, h, pos->st->pawn_key, MOVE_NONE, depth, 2, cont_hist);
        const int base = checkers(pos) != 0 ? MP_EVASION_TT
                       : depth > 0          ? MP_MAIN_TT
                                            : MP_QSEARCH_TT;
        mp.stage = base + 1;  // no usable TT move
    }

    size_t n = 0;
    Move m;
    while ((m = movepick_next(&mp)) != MOVE_NONE) {
        if (n >= cap)
            return cap + 1;
        out[n++] = m;
        if (skip_after >= 0 && (long) n == skip_after)
            movepick_skip_quiets(&mp);
    }
    return n;
}

static void test_movepick_poison(void) {
    banner("movepick 0xAA construction poison");

    // One quiet middlegame, one in-check position (evasions), one tactical mess.
    static const char *const fens[] = {
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -",
        "rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq -",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - -",
    };
    // depth > 0 main stage, depth 0 qsearch stage, probcut, and a skip-quiets
    // walk through the main stage.
    static const struct {
        int depth;
        bool probcut;
        long skip_after;
    } modes[] = { { 8, false, -1 }, { 0, false, -1 }, { 0, true, -1 }, { 8, false, 3 } };

    enum { CAP = MAX_MOVES + 4 };

    for (size_t i = 0; i < sizeof fens / sizeof fens[0]; ++i) {
        Position pos;
        StateInfo st;
        CHECK(pos_set(&pos, fens[i], false, &st), "setup %zu", i);

        for (size_t mode = 0; mode < sizeof modes / sizeof modes[0]; ++mode) {
            // Three fills: 0xAA and 0x55 are the two poisons (they differ in
            // EVERY bit, so a field read before the constructor writes it
            // diverges between them -- including a bool, which clang truncates
            // to its low bit, making any single even pattern read as `false`),
            // and 0x00 is the reference the removed zero-fill used to provide.
            Move a[CAP], b[CAP], c[CAP];
            const size_t na = drain_picker(0xAA, &pos, modes[mode].depth, modes[mode].probcut,
                                           modes[mode].skip_after, a, CAP);
            const size_t nb = drain_picker(0x55, &pos, modes[mode].depth, modes[mode].probcut,
                                           modes[mode].skip_after, b, CAP);
            const size_t nc = drain_picker(0x00, &pos, modes[mode].depth, modes[mode].probcut,
                                           modes[mode].skip_after, c, CAP);

            CHECK(na <= CAP && nb <= CAP, "poisoned picker ran away, fen %zu mode %zu", i, mode);
            CHECK(na == nc && nb == nc,
                  "poison changed the move count: %zu / %zu vs %zu, fen %zu mode %zu", na, nb, nc,
                  i, mode);
            if (na == nc && nb == nc && nc <= CAP)
                for (size_t k = 0; k < nc; ++k)
                    CHECK(a[k] == c[k] && b[k] == c[k],
                          "poison changed move %zu: %04x / %04x vs %04x, fen %zu mode %zu", k, a[k],
                          b[k], c[k], i, mode);

            // The full main/evasion drain must return every pseudo-legal move
            // exactly once -- the picker invariant the stages exist to keep.
            if (!modes[mode].probcut && modes[mode].depth > 0 && modes[mode].skip_after < 0) {
                ExtMove all[MAX_MOVES];
                const size_t total =
                  (size_t) (generate(&pos, all,
                                     checkers(&pos) != 0 ? GEN_EVASIONS : GEN_NON_EVASIONS)
                            - all);
                CHECK(na == total, "picker drained %zu of %zu pseudo-legal moves, fen %zu", na,
                      total, i);
                for (size_t g = 0; g < total && na == total; ++g) {
                    size_t seen = 0;
                    for (size_t k = 0; k < na; ++k)
                        seen += (size_t) (a[k] == all[g].move);
                    CHECK(seen == 1, "move %04x returned %zu times, fen %zu", all[g].move, seen, i);
                }
            }
        }
    }
}

// FNV-1a over a weight block, so two parses into the same storage can be
// compared byte-for-byte without keeping a 106 MB copy.
static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

static void test_nnue_parse_poison(void) {
    banner("nnue weight-arena 0xAA parse poison");

    if (!eval_nnue_init()) {
        CHECK(false, "eval_nnue_init failed");
        return;
    }
    if (!eval_nnue_load("resources/", nullptr)) {
        // The net is a runtime input; without it there is no parse to audit.
        // Say so loudly -- a skipped check proves nothing.
        printf("  SKIP: resources/%s not present; parse-poison audit not run\n",
               eval_nnue_default_file_name());
        return;
    }

    static const char *const fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -",
        "4rrk1/pp1n3p/3q2pQ/2p1pb2/2PP4/2P3N1/P2B2PP/4RRK1 b - -",
    };

    Value before[sizeof fens / sizeof fens[0]];
    for (size_t i = 0; i < sizeof fens / sizeof fens[0]; ++i) {
        Position pos;
        StateInfo st;
        CHECK(pos_set(&pos, fens[i], false, &st), "setup %zu", i);
        before[i] = evaluate(&pos);
    }

    // Checksum every block the first parse produced: the FT arena plus the 48
    // per-(bucket, layer, part) affine blocks.
    uint64_t sum_ft = fnv1a(nnue_ft_ptr(), NNUE_FT_TOTAL_BYTES);
    uint64_t sum_layer[NNUE_LAYER_STACKS][NNUE_LAYERS_PER_STACK][2];
    for (size_t bucket = 0; bucket < NNUE_LAYER_STACKS; ++bucket)
        for (size_t idx = 0; idx < NNUE_LAYERS_PER_STACK; ++idx) {
            sum_layer[bucket][idx][0] =
              fnv1a(nnue_layer_ptr(bucket, idx, NNUE_LAYER_WEIGHTS), nnue_layer_weights_bytes(idx));
            sum_layer[bucket][idx][1] =
              fnv1a(nnue_layer_ptr(bucket, idx, NNUE_LAYER_BIASES), nnue_layer_biases_bytes(idx));
        }

    // Poison the whole arena, then force a re-parse over it. The storage
    // getters return the existing blocks, so the second parse writes into the
    // poisoned memory; any byte the parse skips but a reader reaches now holds
    // 0xAA instead of whatever the first allocation happened to contain.
    memset(nnue_ft_storage(NNUE_FT_TOTAL_BYTES), 0xAA, NNUE_FT_TOTAL_BYTES);
    for (size_t bucket = 0; bucket < NNUE_LAYER_STACKS; ++bucket)
        for (size_t idx = 0; idx < NNUE_LAYERS_PER_STACK; ++idx) {
            memset(
              nnue_layer_storage(bucket, idx, NNUE_LAYER_WEIGHTS, nnue_layer_weights_bytes(idx)),
              0xAA, nnue_layer_weights_bytes(idx));
            memset(nnue_layer_storage(bucket, idx, NNUE_LAYER_BIASES, nnue_layer_biases_bytes(idx)),
                   0xAA, nnue_layer_biases_bytes(idx));
        }

    // Clear the loaded-net identity so network_load parses again instead of
    // taking its wanted-net-is-resident early-out.
    nnue_set_loaded_state("", 0, "", 0);
    CHECK(eval_nnue_load("resources/", nullptr), "re-parse over the poisoned arena");

    // The parse is deterministic, so every block must come back byte-identical:
    // a byte the parse failed to write survives as 0xAA and breaks its checksum.
    CHECK(fnv1a(nnue_ft_ptr(), NNUE_FT_TOTAL_BYTES) == sum_ft,
          "FT arena differs after the poisoned re-parse");
    for (size_t bucket = 0; bucket < NNUE_LAYER_STACKS; ++bucket)
        for (size_t idx = 0; idx < NNUE_LAYERS_PER_STACK; ++idx) {
            CHECK(
              fnv1a(nnue_layer_ptr(bucket, idx, NNUE_LAYER_WEIGHTS), nnue_layer_weights_bytes(idx))
                == sum_layer[bucket][idx][0],
              "fc_%zu weights bucket %zu differ after the poisoned re-parse", idx, bucket);
            CHECK(
              fnv1a(nnue_layer_ptr(bucket, idx, NNUE_LAYER_BIASES), nnue_layer_biases_bytes(idx))
                == sum_layer[bucket][idx][1],
              "fc_%zu biases bucket %zu differ after the poisoned re-parse", idx, bucket);
        }

    // And the evaluation itself must not move.
    for (size_t i = 0; i < sizeof fens / sizeof fens[0]; ++i) {
        Position pos;
        StateInfo st;
        CHECK(pos_set(&pos, fens[i], false, &st), "re-setup %zu", i);
        CHECK(evaluate(&pos) == before[i], "eval moved after the poisoned re-parse, fen %zu", i);
    }
}

// ------------------------------------------------------------ numa topology

// Parse S and report the resulting node/CPU shape. Every case below is stated as what
// upstream's NumaConfig::from_string answers for the same string, because a policy the
// two engines read differently is a topology they run differently.
static bool parse_policy(const char *s, size_t *out_nodes, size_t *out_cpus) {
    NumaConfig cfg;
    if (!numa_config_from_string(&cfg, s, strlen(s)))
        return false;
    *out_nodes = numa_config_num_nodes(&cfg);
    *out_cpus = numa_config_num_cpus(&cfg);
    numa_config_destroy(&cfg);
    return true;
}

// ------------------------------------------- nnue accumulator update paths

// Kiwipete: 32 pieces, both kings still on e1/e8 with castling rights, which is what
// makes every one of the four ways up to the top slot reachable from one fixture.
static const char *const AccFen =
  "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

// Walk MOVES through the accumulator bracket and compare the incremental evaluation
// against a full refresh of the same position.
//
// The control is a second arena that is reset before every evaluation, so its top
// slot has no previous ply to walk from and it must rebuild from the board. Any
// disagreement is the incremental path describing a position the board does not
// hold -- which is a class of bug that perft cannot see (it never evaluates), that
// the unit suite could not see before this, and that the bench anchor sees only if
// it happens to fire on one of 51 fixed positions.
//
// EVAL_STRIDE above 1 leaves plies uncomputed between evaluations, which is what
// makes a walk cover a SUFFIX rather than a single ply. The first ply is always
// evaluated whatever the stride: a walk that starts at the never-computed root slot
// can only refresh, so nothing but the refresh path is reachable until one
// evaluation has happened.
static void acc_walk(EvalArena *inc,
                     EvalArena *ctrl,
                     const char *const *moves,
                     size_t eval_stride,
                     const char *label) {
    Position pos;
    StateInfo root_st;
    if (!pos_set(&pos, AccFen, false, &root_st)) {
        CHECK(false, "acc walk %s: fixture setup", label);
        return;
    }

    StateInfo st[16];
    Move played[16];
    size_t depth = 0;

    eval_acc_reset(inc);

    for (size_t i = 0; moves[i] != nullptr && depth < 16; ++i) {
        const Move m = move_from_uci(&pos, moves[i]);
        if (m == MOVE_NONE) {
            CHECK(false, "acc walk %s: %s is not legal here", label, moves[i]);
            break;
        }
        DirtyPiece *dp;
        DirtyThreats *dts;
        eval_acc_push(inc, &dp, &dts);
        pos_do_move(&pos, m, &st[depth], pos_gives_check(&pos, m), dp, dts, nullptr);
        played[depth++] = m;

        const bool evaluate_here = i == 0 || moves[i + 1] == nullptr || (i + 1) % eval_stride == 0;
        if (!evaluate_here)
            continue;

        char fen[128];
        pos_fen(&pos, fen);
        Position fresh;
        StateInfo fresh_st;
        CHECK(pos_set(&fresh, fen, false, &fresh_st), "acc walk %s: round-trip %s", label, fen);
        eval_acc_reset(ctrl);

        const Value incremental = evaluate_nnue_with_optimism(inc, &pos, 0);
        const Value refreshed = evaluate_nnue_with_optimism(ctrl, &fresh, 0);
        CHECK(incremental == refreshed,
              "acc walk %s ply %zu (%s): incremental %d != refresh %d [%s]", label, depth, moves[i],
              incremental, refreshed, fen);
    }

    while (depth > 0) {
        pos_undo_move(&pos, played[--depth]);
        eval_acc_pop(inc);
    }
}

static void test_nnue_accumulator_paths(void) {
    banner("nnue accumulator update paths");

    if (!eval_nnue_init()) {
        CHECK(false, "eval_nnue_init failed");
        return;
    }
    if (!eval_nnue_load("resources/", nullptr)) {
        // The net is a runtime input; without it the NNUE paths do not run at all.
        // Say so loudly -- a skipped check proves nothing.
        printf("  SKIP: resources/%s not present; accumulator paths not exercised\n",
               eval_nnue_default_file_name());
        return;
    }

    EvalArena *const inc = eval_arena_create();
    EvalArena *const ctrl = eval_arena_create();
    if (inc == nullptr || ctrl == nullptr) {
        CHECK(false, "two eval arenas allocate");
        eval_arena_destroy(inc);
        eval_arena_destroy(ctrl);
        return;
    }
    eval_arena_clear_refresh_cache(inc);
    eval_arena_clear_refresh_cache(ctrl);

    // Quiet moves only: neither king moves, so both perspectives stay computed and
    // the shared both-perspectives walk takes every ply.
    static const char *const quiet[] = { "d5e6", "e7e6", "e2a6", "e6e5", nullptr };
    // Same-half king moves at full material: the hybrid step's whole condition.
    static const char *const same_half[] = { "e1f1", "e8f8", "f1e1", "f8e8", nullptr };
    // A king move ACROSS the centre file changes the threat orientation too, and
    // castling also relocates a rook; both must fall back to a full refresh.
    static const char *const cross_half[] = { "e1d1", "e8d8", nullptr };
    static const char *const castles[] = { "e1g1", "e8c8", nullptr };

    // Assert only on counters the INCREMENTAL arena can move. The control arena
    // refreshes twice on purpose before every comparison, so `refresh` carries both
    // arenas' work and says nothing about the path under test; `hybrid` is reachable
    // from the incremental walk alone, and "did not take the hybrid" is the same
    // claim as "fell back to a refresh" seen from the side the counter can see.
#ifdef MCFISH_ACC_STATS
    nnue_acc_stats_reset();
    const NnueAccStats *const s = nnue_acc_stats();
    uint64_t hybrid_before = 0;
#endif

    acc_walk(inc, ctrl, quiet, 1, "quiet");
#ifdef MCFISH_ACC_STATS
    CHECK(s->shared_walk > 0 && s->shared_step > 0,
          "a king-free walk takes the shared both-perspectives pass (%llu walks, %llu steps)",
          (unsigned long long) s->shared_walk, (unsigned long long) s->shared_step);
    hybrid_before = s->hybrid;
#endif

    acc_walk(inc, ctrl, same_half, 1, "same-half king moves");
#ifdef MCFISH_ACC_STATS
    // Three, not four: the first ply cannot use the step because the walk begins at
    // a root slot that was never computed, and the hybrid needs the PREVIOUS ply's
    // accumulation to carry the threat and pair rows forward. The remaining three
    // king moves each take it, for the moving side's own perspective.
    CHECK(s->hybrid - hybrid_before == 3,
          "the same-half fixture takes the hybrid step three times, got %llu",
          (unsigned long long) (s->hybrid - hybrid_before));
    hybrid_before = s->hybrid;
#endif

    acc_walk(inc, ctrl, cross_half, 1, "cross-half king moves");
#ifdef MCFISH_ACC_STATS
    CHECK(s->hybrid == hybrid_before,
          "a king move across the centre file must not take the hybrid step (%llu did)",
          (unsigned long long) (s->hybrid - hybrid_before));
#endif

    acc_walk(inc, ctrl, castles, 1, "castling");
#ifdef MCFISH_ACC_STATS
    CHECK(s->hybrid == hybrid_before, "castling must not take the hybrid step (%llu did)",
          (unsigned long long) (s->hybrid - hybrid_before));
#endif

    // Deferred evaluation: plies land uncomputed between evaluations, so a walk
    // catches up over a suffix and the two perspectives can start from different
    // plies -- the case the shared pass's catch-up loops exist for.
#ifdef MCFISH_ACC_STATS
    const uint64_t shared_step_before = s->shared_step;
    const uint64_t shared_walk_before = s->shared_walk;
#endif
    acc_walk(inc, ctrl, quiet, 3, "quiet, deferred");
    acc_walk(inc, ctrl, same_half, 3, "same-half king moves, deferred");

#ifdef MCFISH_ACC_STATS
    CHECK(s->shared_step - shared_step_before > s->shared_walk - shared_walk_before,
          "a deferred walk carries more than one ply per pass (%llu steps over %llu passes)",
          (unsigned long long) (s->shared_step - shared_step_before),
          (unsigned long long) (s->shared_walk - shared_walk_before));
    CHECK(s->split_walk > 0, "a king move splits the two perspectives' walks");
    printf("  paths: shared %llu (%llu steps), split %llu, hybrid %llu, refresh %llu\n",
           (unsigned long long) s->shared_walk, (unsigned long long) s->shared_step,
           (unsigned long long) s->split_walk, (unsigned long long) s->hybrid,
           (unsigned long long) s->refresh);
#else
    CHECK(false, "the suite must be built with -DMCFISH_ACC_STATS to gate path coverage");
#endif

    eval_arena_destroy(inc);
    eval_arena_destroy(ctrl);
}

// ------------------------------------------ nnue shared pawn-pair producer

// Assert the both-perspectives pawn-pair producer is list-for-list identical to two
// per-perspective calls.
//
// It exists only to pay for the pawn geometry once, so "same indices in the same
// order" is the entire contract -- and an ordering slip between the two would be
// invisible to any value gate, because the accumulator sums the rows and integer
// addition does not care what order they arrive in. It would surface only later, as
// a list that overflows or a pair emitted twice.
static void check_pair_both(uint8_t white_ksq,
                            uint8_t black_ksq,
                            uint64_t white_before,
                            uint64_t black_before,
                            uint64_t white_after,
                            uint64_t black_after,
                            const char *label,
                            size_t *emitted) {
    const NnueDirtyPawnPairs diff = {
        .before = { white_before, black_before },
        .after = { white_after, black_after },
    };

    uint32_t one[4][NNUE_THREAT_INDEX_CAPACITY];
    uint32_t both[4][NNUE_THREAT_INDEX_CAPACITY];
    size_t one_len[4] = { 0, 0, 0, 0 };
    size_t both_len[4] = { 0, 0, 0, 0 };

    // Order: white removed, white added, black removed, black added.
    nnue_pair_append_changed(WHITE, white_ksq, &diff, one[0], &one_len[0], one[1], &one_len[1]);
    nnue_pair_append_changed(BLACK, black_ksq, &diff, one[2], &one_len[2], one[3], &one_len[3]);
    nnue_pair_append_changed_both(white_ksq, black_ksq, &diff, both[0], &both_len[0], both[1],
                                  &both_len[1], both[2], &both_len[2], both[3], &both_len[3]);

    static const char *const names[4] = { "white removed", "white added", "black removed",
                                          "black added" };
    for (size_t i = 0; i < 4; ++i) {
        CHECK(one_len[i] == both_len[i], "%s: %s length %zu vs %zu", label, names[i], one_len[i],
              both_len[i]);
        if (one_len[i] == both_len[i]) {
            CHECK(memcmp(one[i], both[i], one_len[i] * sizeof one[i][0]) == 0,
                  "%s: %s indices differ", label, names[i]);
        }
        *emitted += one_len[i];
    }
}

static void test_nnue_pair_changed_both(void) {
    banner("nnue shared pawn-pair producer");

    // PawnPairBB is built by nnue_feature_init, which eval_nnue_init runs.
    if (!eval_nnue_init()) {
        CHECK(false, "eval_nnue_init failed");
        return;
    }

    // Two adjacent files of pawns on both sides, so the pair bands are populated and
    // a single changed pawn produces several pairs rather than none.
    const uint64_t w = square_bb(SQ_D2) | square_bb(SQ_E2) | square_bb(SQ_F3) | square_bb(SQ_C4);
    const uint64_t b = square_bb(SQ_D7) | square_bb(SQ_E6) | square_bb(SQ_F7) | square_bb(SQ_C5);

    size_t emitted = 0;
    check_pair_both(SQ_G1, SQ_G8, w, b, w, b, "no pawn moved", &emitted);
    CHECK(emitted == 0, "an unchanged pawn set emits nothing, got %zu", emitted);

    // A push, a capture (the white pawn lands on the black pawn's square), a
    // promotion (the pawn leaves the pair space), and a mirrored king bucket.
    check_pair_both(SQ_G1, SQ_G8, w, b, (w ^ square_bb(SQ_E2)) | square_bb(SQ_E4), b, "white push",
                    &emitted);
    check_pair_both(SQ_G1, SQ_G8, w, b, (w ^ square_bb(SQ_C4)) | square_bb(SQ_C5),
                    b ^ square_bb(SQ_C5), "white captures", &emitted);
    check_pair_both(SQ_G1, SQ_G8, w, b, w ^ square_bb(SQ_D2), b, "white pawn disappears", &emitted);
    check_pair_both(SQ_B1, SQ_B8, w, b, w, (b ^ square_bb(SQ_E6)) | square_bb(SQ_E5),
                    "queen-side king bucket", &emitted);
    check_pair_both(SQ_A1, SQ_H8, w, b, (w ^ square_bb(SQ_F3)) | square_bb(SQ_F4),
                    (b ^ square_bb(SQ_D7)) | square_bb(SQ_D5), "both sides moved", &emitted);

    CHECK(emitted > 0, "the fixtures produce pair indices at all");
}

// ------------------------------------------------- search step formulas

// Pin the two search margins this tree most recently took from upstream, at the
// boundaries their formulas turn on. Both are pure integer functions, so the values
// belong in the suite rather than only in a node count that cannot say WHICH term
// moved when it changes.
static void test_search_step_margins(void) {
    banner("search step margins");

    // R = 7 + depth / 3 + max((staticEval - beta) / 256, 0).
    CHECK(null_move_reduction(9, 0, 0) == 10, "base R at depth 9, got %d",
          null_move_reduction(9, 0, 0));
    CHECK(null_move_reduction(9, 255, 0) == 10, "an excess under 256 adds nothing, got %d",
          null_move_reduction(9, 255, 0));
    CHECK(null_move_reduction(9, 256, 0) == 11, "256 above beta adds one, got %d",
          null_move_reduction(9, 256, 0));
    CHECK(null_move_reduction(9, 700, 100) == 12, "the excess is measured from beta, got %d",
          null_move_reduction(9, 700, 100));
    // The guard on beta lets the null move run below the static eval, where the
    // truncating division would otherwise contribute a negative reduction.
    CHECK(null_move_reduction(9, -5000, 0) == 10, "a static eval below beta never shortens R");

    // clamp(delta * singularDepth * 177 / 1024, +/- CORRECTION_HISTORY_LIMIT / 4).
    CHECK(multicut_correction_bonus(0, 8) == 0, "no delta, no bonus");
    CHECK(multicut_correction_bonus(64, 4) == 64 * 4 * 177 / 1024, "the unclamped body, got %d",
          multicut_correction_bonus(64, 4));
    CHECK(multicut_correction_bonus(30000, 60) == CORRECTION_HISTORY_LIMIT / 4,
          "clamped above at a quarter of the limit, got %d", multicut_correction_bonus(30000, 60));
    CHECK(multicut_correction_bonus(-30000, 60) == -CORRECTION_HISTORY_LIMIT / 4,
          "clamped below at a quarter of the limit, got %d", multicut_correction_bonus(-30000, 60));
}

// ------------------------------------------------- the root PV capacity contract

// Pin what separates a ROOT PV from a per-ply one.
//
// `PVMoves` is a fixed MAX_PLY + 1 buffer and that bound is exact for a line the
// SEARCH produced. A root move's PV is not only search-produced: syzygy_extend_pv
// walks on past it toward a tablebase mate, and a six-man DTZ line is longer than
// the recursion ever is. A shared type either overruns on that line or truncates
// it, and neither is a report of what the tablebase proved.
//
// No gate here reaches the walk itself -- `tb-fetch` installs three-man tables and
// `tb-fetch 5` five-man ones, and neither holds a DTZ line that long -- so this
// drives the TYPE, which is where the contract lives.
static void test_root_pv_capacity(void) {
    banner("root PV capacity");

    RootPVMoves pv;
    CHECK(root_pv_init(&pv), "a root PV reserves its search-length buffer");
    CHECK(pv.capacity >= (size_t) ROOT_PV_SEARCH_CAP, "reserved at least the search bound, got %zu",
          pv.capacity);
    CHECK(pv.length == 0, "and starts empty");

    // A search-produced line: the root move plus a full-length child PV. This must
    // fit the reservation, which is what lets root_pv_set_line be total.
    Move child[MAX_PLY];
    for (size_t i = 0; i < MAX_PLY; ++i)
        child[i] = make_move((Square) (i % 64), (Square) ((i + 1) % 64));
    root_pv_set_line(&pv, child[0], child, MAX_PLY);
    CHECK(pv.length == (size_t) MAX_PLY + 1, "the longest search line fits exactly, got %zu",
          pv.length);
    CHECK(pv.moves[0] == child[0] && pv.moves[MAX_PLY] == child[MAX_PLY - 1],
          "and both ends survive the copy");

    // PAST the search bound. This is the shape the tablebase walk produces and the
    // one the old fixed buffer could not hold.
    const size_t extra = 400;
    for (size_t i = 0; i < extra; ++i)
        CHECK(root_pv_push(&pv, child[i % MAX_PLY]), "a DTZ move appends past the search bound");
    CHECK(pv.length == (size_t) MAX_PLY + 1 + extra, "every appended move is in the line, got %zu",
          pv.length);
    CHECK(pv.capacity >= pv.length, "and the capacity grew to hold them");

    // The copy that saves and restores a best line must carry the whole thing,
    // growing a destination that was only reserved for a search line.
    RootPVMoves saved;
    CHECK(root_pv_init(&saved), "the save buffer reserves too");
    root_pv_copy(&saved, &pv);
    CHECK(saved.length == pv.length, "a save keeps the extended line whole, got %zu vs %zu",
          saved.length, pv.length);
    CHECK(memcmp(saved.moves, pv.moves, saved.length * sizeof *saved.moves) == 0,
          "and every move of it");

    // Narrowing to a per-ply PV is the one crossing between the two contracts, and
    // it truncates rather than overruns.
    PVMoves narrow;
    pv_from_root(&narrow, &pv);
    CHECK(narrow.length == (size_t) MAX_PLY + 1, "narrowing stops at the per-ply bound, got %zu",
          narrow.length);
    CHECK(narrow.moves[MAX_PLY] == pv.moves[MAX_PLY], "keeping the prefix it did copy");

    root_pv_truncate(&pv, 3);
    CHECK(pv.length == 3, "a truncation shrinks, got %zu", pv.length);
    root_pv_truncate(&pv, 900);
    CHECK(pv.length == 3, "and never grows");

    root_pv_release(&saved);
    root_pv_release(&pv);
    CHECK(pv.moves == nullptr && pv.length == 0 && pv.capacity == 0, "a released PV holds nothing");
    root_pv_release(&pv);  // idempotent
}

// ------------------------------------------------- time budget on a zero clock

// Pin what the budget answers when the side to move has no clock.
//
// `use_time_management` is true when EITHER side's clock is set, so `go wtime 0
// btime N` runs a managed search whose own clock is zero -- and both bounds are
// then read, by the per-node maximum check and by the iteration's optimum
// scaling. Before upstream 92c90f41e neither had a value on that path: upstream
// left them indeterminate, and this tree passed the PREVIOUS `go`'s budget
// through, so a search after a timed one inherited its deadline. The bound is
// the property; the specific sentinel is not, so assert the shape -- a value far
// past any real clock, and independent of what came before.
static void test_timeman_zero_clock(void) {
    banner("time budget on a zero clock");

    const TimemanInput zero = {
        .time = 0,
        .inc = 0,
        .start_time = 1000,
        .npmsec = 0,
        .move_overhead = 10,
        .available_nodes = -1,
        // What a previous `go` would have left behind: a one-second budget.
        .current_optimum_time = 500,
        .current_maximum_time = 1000,
        .movestogo = 0,
        .ply = 0,
        .original_time_adjust = -1.0,
        .ponder = false,
    };
    const TimemanOutput out = timeman_compute(zero);
    CHECK(out.optimum_time == TIMEMAN_NO_BOUND,
          "a zero clock bounds the optimum at NO_BOUND, got %lld", (long long) out.optimum_time);
    CHECK(out.maximum_time == TIMEMAN_NO_BOUND,
          "a zero clock bounds the maximum at NO_BOUND, got %lld", (long long) out.maximum_time);
    CHECK(out.start_time == 1000, "the start time still comes through");

    // A real clock still computes a real budget, and one well under the sentinel.
    TimemanInput real = zero;
    real.time = 60000;
    double adjust = -1.0;
    real.original_time_adjust = adjust;
    const TimemanOutput got = timeman_compute(real);
    CHECK(got.optimum_time > 0 && got.optimum_time < TIMEMAN_NO_BOUND,
          "a real clock computes a real optimum, got %lld", (long long) got.optimum_time);
    CHECK(got.maximum_time >= got.optimum_time, "the maximum is never below the optimum");

    // timeman_clear must establish the same two bounds, or the very first `go` of
    // a game reads whatever the block was allocated with.
    TimeManagement tm = { .optimum_time = 7, .maximum_time = 9 };
    timeman_clear(&tm);
    CHECK(tm.optimum_time == TIMEMAN_NO_BOUND && tm.maximum_time == TIMEMAN_NO_BOUND,
          "a cleared manager carries no budget from the last game");
    CHECK(tm.available_nodes == -1, "and no node budget either");
}

// ------------------------------------------------- syzygy WDL score domain

// Pin the domain `wdl.h` promises for a WDL probe: a score in -2..2.
//
// The value a probe returns is one the FILE decided -- a btree leaf on the
// compressed path, `min_sym_len` verbatim on the single-value one -- and until
// `do_probe_table` bounded it, nothing between the mapped bytes and the score
// did. A stored byte of 255 left the probe as a WDL score of 253, and the root
// ranking then read `WdlToRank[wdl + 2]` five entries wide.
//
// Construct the table rather than corrupt a file: this is the one place the
// whole hostile-file path can be driven with no mapping, no registry generation
// and no dependence on which table the fetch step happened to install. The
// single-value branch is the shortest route to a value the file chose, because
// `decode_pairs` returns `min_sym_len` before it reads a single compressed bit.
static void test_syzygy_wdl_score_domain(void) {
    banner("syzygy WDL score domain");

    encode_init_geometry();

    // KQvK with white to move: no pawns, three men, and a unique piece, so the
    // probe takes the piece-encoding branch and never asks for a lead pawn.
    Position pos;
    StateInfo si;
    if (!pos_set(&pos, "8/8/8/8/8/2K5/3Q4/7k w - - 0 1", false, &si)) {
        CHECK(false, "KQvK fen must parse");
        return;
    }

    TBTable t;
    memset(&t, 0, sizeof t);
    t.key = syzygy_position_key(&pos);
    t.key2 = t.key + 1;  // not symmetric, so a white-to-move probe takes side 0
    t.piece_count = 3;
    t.has_pawns = false;
    t.has_unique_pieces = true;
    t.sides = 2;

    PairsData *const d = tbtable_get(&t, false, TB_STM_WHITE, TB_FILE_A);
    d->flags = TB_FLAG_SINGLE_VALUE;
    d->group_len[0] = 3;  // all three men in one group; group_len[1] == 0 ends the walk
    d->group_idx[0] = 1;

    // Every value a WDL file can hold survives, mapped to its score.
    for (int32_t stored = 0; stored <= 4; ++stored) {
        d->min_sym_len = (uint8_t) stored;
        int32_t state = PROBE_OK;
        const int32_t score = do_probe_table(&pos, &t, false, 0, &state);
        CHECK(state != PROBE_FAIL, "stored %d is a WDL outcome and must probe", stored);
        CHECK(score == stored - 2, "stored %d maps to score %d, got %d", stored, stored - 2, score);
    }

    // Nothing else does. 255 is the byte that reached the ranking as -253.
    static constexpr uint8_t Invented[] = { 5, 6, 127, 128, 200, 255 };
    for (size_t i = 0; i < sizeof Invented / sizeof Invented[0]; ++i) {
        d->min_sym_len = Invented[i];
        int32_t state = PROBE_OK;
        const int32_t score = do_probe_table(&pos, &t, false, 0, &state);
        CHECK(state == PROBE_FAIL, "stored %u is no WDL outcome and must be refused",
              (unsigned) Invented[i]);
        CHECK(score == 0, "a refused probe yields 0, got %d", score);
    }
}

// Hold the tail of hash_bytes to a digest that uses every byte it was given.
//
// Upstream reads the tail through a SIGNED char, so a byte >= 0x80 becomes
// 0xFFFFFFFFFFFFFF00 | byte and the `or` sets every bit above bit 7 -- erasing
// what the higher tail indices already contributed. The symptom is a collapsed
// range rather than a crash, which is why no other gate here can see it: nothing
// in this tree reads the digest today, and a defect nothing reads is one a future
// caller inherits silently.
//
// The census is the assertion, not a spot value: over all 65536 two-byte inputs a
// 64-bit hash must produce 65536 distinct digests, and the signed read produces
// 32896.
static void test_nnue_hash_tail_bytes(void) {
    // Every two-byte input, deduplicated by sorting the digests.
    static uint64_t seen[65536];
    for (uint32_t v = 0; v < 65536; ++v) {
        const uint8_t in[2] = { (uint8_t) (v & 0xFF), (uint8_t) (v >> 8) };
        seen[v] = nnue_hash_bytes(in, sizeof in);
    }
    qsort(seen, 65536, sizeof seen[0], cmp_u64);
    size_t distinct = 1;
    for (size_t i = 1; i < 65536; ++i)
        if (seen[i] != seen[i - 1])
            ++distinct;
    CHECK(distinct == 65536, "hash_bytes: %zu distinct digests over 65536 two-byte inputs",
          distinct);

    // The named collision from the register, which is the shape a reader can check
    // by hand: the two differ only in a byte AFTER a 0x80, which sign extension
    // erases.
    const uint8_t a[3] = { 0x80, 0x02, 0x03 };
    const uint8_t b[3] = { 0x80, 0x02, 0x04 };
    CHECK(nnue_hash_bytes(a, sizeof a) != nnue_hash_bytes(b, sizeof b),
          "hash_bytes: {80,02,03} and {80,02,04} collide");

    // A tail with no high bit set must be untouched by the change, which is what
    // says this is a fix to the sign extension and not to the algorithm. The
    // constant is REFERENCE MurmurHash64A over the same three bytes, computed
    // independently of this implementation -- pinning what mcfish happens to
    // print would only photograph the current behaviour.
    const uint8_t c[3] = { 0x01, 0x02, 0x03 };
    CHECK(nnue_hash_bytes(c, sizeof c) == 0xf187641bcbc5b011ULL,
          "hash_bytes: diverged from reference MurmurHash64A on a plain tail");
}

static void test_numa_from_string(void) {
    banner("numa policy strings");

    size_t nodes = 0, cpus = 0;

    CHECK(parse_policy("0-3,8:4-7", &nodes, &cpus), "0-3,8:4-7 parses");
    CHECK(nodes == 2 && cpus == 9, "0-3,8:4-7 -> 2 nodes / 9 cpus, got %zu / %zu", nodes, cpus);

    CHECK(parse_policy("0-7:8-15", &nodes, &cpus), "0-7:8-15 parses");
    CHECK(nodes == 2 && cpus == 16, "0-7:8-15 -> 2 nodes / 16 cpus, got %zu / %zu", nodes, cpus);

    // An empty node segment is skipped without consuming a node index (numa.h:674).
    CHECK(parse_policy("0-1::2-3", &nodes, &cpus), "empty segment is skipped");
    CHECK(nodes == 2 && cpus == 4, "0-1::2-3 -> 2 nodes, got %zu", nodes);

    // A string naming no node at all is REFUSED; upstream returns nullopt on n == 0
    // (numa.h:686) and the caller keeps the previous topology.
    CHECK(!parse_policy("", &nodes, &cpus), "empty string is refused");
    CHECK(!parse_policy(":::", &nodes, &cpus), "only-separators string is refused");
    CHECK(!parse_policy("abc", &nodes, &cpus), "unparseable string is refused");

    // A CPU may belong to at most one node, and add_cpu_to_node refuses ANY re-add
    // (numa.h:995) -- including one back into the node that already holds it.
    CHECK(!parse_policy("0,0", &nodes, &cpus), "duplicate cpu in one node is refused");
    CHECK(!parse_policy("0-3:2", &nodes, &cpus), "cpu claimed by two nodes is refused");

    // A malformed ELEMENT fails the WHOLE string, which is this reader's one deliberate
    // divergence from upstream: indices_from_shortened_string never fails (numa.h:1033),
    // so upstream reads each of these in part and installs a topology nobody asked for
    // with nothing said. The caller's "keeping previous config" path is what a refusal
    // reaches. These three cases used to assert the lenient reading.
    CHECK(!parse_policy("0-1,7-3", &nodes, &cpus), "reversed range fails the string");
    CHECK(!parse_policy("0-1,x", &nodes, &cpus), "unparseable element fails the string");
    CHECK(!parse_policy("0-1,1-2-3", &nodes, &cpus), "three-part element fails the string");

    // The tail after the digits must be whitespace in FULL, not just its first byte:
    // upstream inspects one character (misc.cpp:557), so "1 2" reads as 1 and
    // `NumaPolicy 0,1 2,3` -- split on `, : -` alone -- installed {0,1,3}.
    CHECK(!parse_policy("0,1 2,3", &nodes, &cpus), "an embedded space fails the string");
    CHECK(parse_policy("0, 1 ,2", &nodes, &cpus), "surrounding whitespace still parses");
    CHECK(nodes == 1 && cpus == 3, "0, 1 ,2 -> 3 cpus, got %zu", cpus);

    // The range cap is upstream's 1 << 20 (numa.h:1053), so a hostile range costs nothing
    // rather than asking for a multi-gigabyte allocation.
    CHECK(!parse_policy("0-4000000000", &nodes, &cpus), "oversized range yields no node");
}

static void test_numa_config_shape(void) {
    banner("numa topology");

    NumaConfig cfg;
    numa_config_init(&cfg);

    CHECK(numa_config_add_cpu_to_node(&cfg, 0, 5) == NUMA_ADD_OK, "add cpu 5 to node 0");
    CHECK(numa_config_add_cpu_to_node(&cfg, 0, 1) == NUMA_ADD_OK, "add cpu 1 to node 0");
    CHECK(numa_config_add_cpu_to_node(&cfg, 1, 5) == NUMA_ADD_CONFLICT, "cpu 5 is taken");
    CHECK(numa_config_add_cpu_to_node(&cfg, 0, 5) == NUMA_ADD_CONFLICT, "re-add is refused");

    size_t count = 0;
    const size_t *list = numa_config_node_cpus(&cfg, 0, &count);
    CHECK(count == 2 && list[0] == 1 && list[1] == 5, "node cpu set stays ascending");

    // A single thread is never distributed, so `auto` never binds it -- this is what keeps
    // the single-threaded path the same shape on every host.
    CHECK(!numa_config_suggests_binding_threads(&cfg, 1), "one thread never binds");

    numa_config_destroy(&cfg);

    // A user-set topology always binds (numa.h:768), whatever the thread count.
    NumaConfig custom;
    CHECK(numa_config_from_string(&custom, "0-7:8-15", 8), "two-node policy parses");
    CHECK(numa_config_suggests_binding_threads(&custom, 2), "custom affinity always binds");

    size_t assigned[8];
    CHECK(numa_config_distribute_threads(&custom, 8, assigned), "distribute 8 threads");
    size_t per_node[2] = { 0, 0 };
    for (size_t i = 0; i < 8; ++i)
        per_node[assigned[i]] += 1;
    CHECK(per_node[0] == 4 && per_node[1] == 4, "8 threads split 4/4, got %zu/%zu", per_node[0],
          per_node[1]);
    numa_config_destroy(&custom);

    // The system topology must always name at least one node holding at least one CPU:
    // every downstream division is by a node's CPU count.
    NumaConfig sys;
    CHECK(numa_config_from_system(&sys, true), "system topology reads");
    CHECK(numa_config_num_nodes(&sys) >= 1, "system topology has a node");
    CHECK(numa_config_num_cpus_in_node(&sys, 0) >= 1, "node 0 holds a cpu");
    numa_config_destroy(&sys);
}

// ---------------------------------------------------------------- thread pool

// Count through an atomic: the pool runs these jobs on four threads AT ONCE, so a plain
// `int` counter here is itself a data race -- and one that hides whether the pool is
// running them concurrently at all.
static void count_job(void *ctx) { atomic_fetch_add((atomic_int *) ctx, 1); }

static bool count_build(void *ctx, size_t idx, Thread *thread) {
    (void) idx;
    atomic_int *built = (atomic_int *) ctx;
    atomic_fetch_add(built, 1);
    thread_set_worker(thread, built);
    return true;
}

static void test_thread_pool(void) {
    banner("thread pool");

    CHECK(next_power_of_two(0) == 1, "0 -> 1");
    CHECK(next_power_of_two(1) == 1, "1 -> 1");
    CHECK(next_power_of_two(3) == 4, "3 -> 4");
    CHECK(next_power_of_two(16) == 16, "16 -> 16");
    CHECK(next_power_of_two(17) == 32, "17 -> 32");

    ThreadPool pool;
    thread_pool_init(&pool);

    atomic_int built = 0;
    ThreadBuilder builder = { &built, count_build, nullptr };
    CHECK(thread_pool_set(&pool, 4, &builder, nullptr, nullptr), "spawn four threads");
    CHECK(thread_pool_num_threads(&pool) == 4, "pool reports four threads");
    CHECK(built == 4, "the builder ran once per thread, got %d", (int) built);

    // Every thread must actually run a submitted job and be waitable, or a search would
    // start siblings that never search and wait on them forever.
    atomic_int ran = 0;
    for (size_t i = 0; i < 4; ++i)
        thread_pool_run_on_thread(&pool, i, count_job, &ran);
    thread_pool_wait_from(&pool, 0);
    CHECK(ran == 4, "every thread ran its job, got %d", (int) ran);

    thread_pool_set_stop(&pool, true);
    CHECK(thread_pool_stopped(&pool), "stop is observable");

    // clear() must be idempotent and safe to repeat: teardown reaches it from both the
    // reconfigure path and the process exit path.
    thread_pool_clear(&pool);
    CHECK(thread_pool_num_threads(&pool) == 0, "clear empties the pool");
    thread_pool_clear(&pool);

    // Churn the pool: construct/destroy is where a teardown race shows up, and a dropped
    // join leaves a thread reading a freed Thread object.
    for (int round = 0; round < 8; ++round) {
        built = 0;
        CHECK(thread_pool_set(&pool, 3, &builder, nullptr, nullptr), "respawn round %d", round);
        // Each thread's job context is its own `worker`, which the builder pointed at
        // `built` -- so this counts three more increments on top of the three builds.
        thread_pool_start_jobs(&pool, count_job, 0);
        thread_pool_wait_from(&pool, 0);
        CHECK(built == 6, "round %d ran three builds and three jobs, got %d", round, (int) built);
        thread_pool_clear(&pool);
    }
    CHECK(thread_pool_num_threads(&pool) == 0, "churn leaves the pool empty");
}

// The LEB128 encoder against the decoder that reads it back, and against the eight
// encodings the format's sign rule fixes.
//
// `net-roundtrip` covers this end to end, but only over ONE net's values and only on
// a machine that has the net; this pins the boundaries a 95 MB file may never contain
// and runs under the sanitizers. The eight literals are the sharp half: the rule that
// a group ending with bit 0x40 set continues unless the remainder is -1 is what
// decides whether 64 takes one byte or two, and getting it wrong produces a file of
// exactly the right LENGTH with the wrong bytes in it.
static void test_nnue_leb_roundtrip(void) {
    banner("nnue LEB128 encode/decode");

    static constexpr int32_t Values[] = { 0,      1,       -1,        63,        64,
                                          -64,    -65,     127,       -128,      8191,
                                          -8192,  8192,    -8193,     INT16_MAX, INT16_MIN,
                                          123456, -123456, INT32_MAX, INT32_MIN };
    static constexpr size_t Count = sizeof Values / sizeof Values[0];

    // Two bytes for 64 and one for -64: the same magnitude, and the sign rule splits
    // them. A run without the continuation bit set encodes both as one byte and every
    // length still matches, which is why the BYTES are asserted and not just the
    // count.
    static constexpr struct {
        int32_t value;
        size_t len;
        uint8_t bytes[2];
    } Fixed[] = {
        { 0, 1, { 0x00, 0x00 } },   { 1, 1, { 0x01, 0x00 } },   { -1, 1, { 0x7f, 0x00 } },
        { 63, 1, { 0x3f, 0x00 } },  { 64, 2, { 0xc0, 0x00 } },  { -64, 1, { 0x40, 0x00 } },
        { -65, 2, { 0xbf, 0x7f } }, { 127, 2, { 0xff, 0x00 } }, { -128, 2, { 0x80, 0x7f } },
    };

    FILE *const mem = tmpfile();
    if (mem == nullptr) {
        CHECK(false, "tmpfile() for the encoder test");
        return;
    }

    for (size_t i = 0; i < sizeof Fixed / sizeof Fixed[0]; ++i) {
        NnueWriter w = { .out = mem, .ok = true };
        const int32_t v = Fixed[i].value;
        rewind(mem);
        nnue_write_leb_i32(&w, &v, 1);
        CHECK(w.ok, "writing %d succeeds", v);
        CHECK(nnue_leb_bytes_i32(&v, 1) == Fixed[i].len, "%d measures %zu bytes", v, Fixed[i].len);

        uint8_t got[4] = { 0 };
        rewind(mem);
        CHECK(fread(got, 1, Fixed[i].len, mem) == Fixed[i].len, "%d reads back its bytes", v);
        CHECK(memcmp(got, Fixed[i].bytes, Fixed[i].len) == 0,
              "%d encodes to the bytes the format fixes", v);
    }

    // int32 values, then the int16-representable ones at the narrower width: the two
    // entry points share one encoder, so this asserts the sign extension a narrower
    // element goes through rather than a second implementation.
    int16_t narrow[Count];
    size_t narrow_count = 0;
    for (size_t i = 0; i < Count; ++i)
        if (Values[i] >= INT16_MIN && Values[i] <= INT16_MAX)
            narrow[narrow_count++] = (int16_t) Values[i];

    NnueWriter w = { .out = mem, .ok = true };
    rewind(mem);
    nnue_write_leb_i32(&w, Values, Count);
    nnue_write_leb_i16(&w, narrow, narrow_count);
    CHECK(w.ok, "the mixed-width run writes");
    fflush(mem);

    const size_t wide_bytes = nnue_leb_bytes_i32(Values, Count);
    const size_t narrow_bytes = nnue_leb_bytes_i16(narrow, narrow_count);
    uint8_t encoded[512] = { 0 };
    CHECK(wide_bytes + narrow_bytes <= sizeof encoded, "the run fits the read buffer");
    rewind(mem);
    CHECK(fread(encoded, 1, wide_bytes + narrow_bytes, mem) == wide_bytes + narrow_bytes,
          "the measured length is the written length");
    fclose(mem);

    int32_t wide_out[Count];
    size_t consumed = 0;
    CHECK(nnue_decode_leb_i32(encoded, wide_bytes, wide_out, Count, &consumed),
          "the int32 run decodes");
    CHECK(consumed == wide_bytes, "the int32 run consumes exactly what it measured");
    for (size_t i = 0; i < Count; ++i)
        CHECK(wide_out[i] == Values[i], "int32 value %zu round-trips (%d != %d)", i, wide_out[i],
              Values[i]);

    int16_t narrow_out[Count];
    consumed = 0;
    CHECK(
      nnue_decode_leb_i16(encoded + wide_bytes, narrow_bytes, narrow_out, narrow_count, &consumed),
      "the int16 run decodes");
    CHECK(consumed == narrow_bytes, "the int16 run consumes exactly what it measured");
    for (size_t i = 0; i < narrow_count; ++i)
        CHECK(narrow_out[i] == narrow[i], "int16 value %zu round-trips (%d != %d)", i,
              (int) narrow_out[i], (int) narrow[i]);
}

int main(void) {
    bitboards_init();
    attacks_init();
    threats_init();
    position_init();

    test_bitboards();
    test_fen();
    test_perft();
    test_fen_parse_robustness();
    test_roundtrip();
    test_deep_line_roundtrip();
    test_null_move();
    test_legality();
    test_uci_move_strings();
    test_evaluate();
    test_tt();
    test_search();
    test_search_reached_positions();
    test_draw_detection();
    test_nnue_dot4();
    test_nnue_accumulator_paths();
    test_nnue_pair_changed_both();
    test_search_step_margins();
    test_root_pv_capacity();
    test_timeman_zero_clock();
    test_movepick_poison();
    test_nnue_parse_poison();
    test_nnue_leb_roundtrip();
    test_syzygy_wdl_score_domain();
    test_nnue_hash_tail_bytes();
    test_numa_from_string();
    test_numa_config_shape();
    test_thread_pool();

    // Release what the search allocated on first use, so the leak checker sees the
    // teardown the process itself runs.
    search_shutdown();
    eval_nnue_shutdown();

    printf("\n%d checks, %d failures\n", Checks, Failures);
    if (Failures) {
        printf("FAILED\n");
        return 1;
    }
    printf("PASSED\n");
    return 0;
}
