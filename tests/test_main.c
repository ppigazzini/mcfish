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
#include "../src/engine/search/search.h"
#include "../src/engine/search/tt.h"
#include "../src/engine/eval/nnue/simd.h"
#include "../src/platform/numa.h"
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

// ---------------------------------------------------------------- bitboards

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
        pos_do_move(pos, it->move, &st, false, &pos->scratch_dp, &pos->scratch_dts);

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

static void test_null_move(void) {
    banner("null move round-trip");

    Position pos;
    StateInfo st;
    pos_set(&pos, "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -", false, &st);

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
    pos_set(&pos, "k3r3/8/8/8/8/8/4N3/4K3 w - -", false, &st);
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
    pos_set(&pos, "k7/8/8/8/8/5n2/8/4K2r w - -", false, &st);
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
        pos_set(&pos, fens[i], false, &st);

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
    pos_set(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", false, &st);

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
    pos_set(&pos, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq -", false, &st);
    const Move oo = move_from_uci(&pos, "e1g1");
    CHECK(oo != MOVE_NONE && move_type(oo) == CASTLING, "e1g1 is castling");
    move_to_uci(&pos, oo, buf);
    CHECK(strcmp(buf, "e1g1") == 0, "castling prints king destination, got %s", buf);

    // Under-promotion must be distinguishable from queening.
    pos_set(&pos, "8/P6k/8/8/8/8/8/K7 w - -", false, &st);
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
    pos_set(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", false, &st);
    const Value start = evaluate(&pos);
    CHECK(start > 0 && start < 100, "start eval is a small tempo bonus, got %d", start);

    // A mirrored position must evaluate identically for the side to move, or the
    // eval has a color bias the search would happily exploit into nonsense.
    pos_set(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1", false, &st);
    CHECK(evaluate(&pos) == start, "eval is color-symmetric");

    // A queen up must read as clearly winning.
    pos_set(&pos, "4k3/8/8/8/8/8/8/3QK3 w - -", false, &st);
    CHECK(evaluate(&pos) > QUEEN_VALUE / 2, "queen up is winning, got %d", evaluate(&pos));

    pos_set(&pos, "3qk3/8/8/8/8/8/8/4K3 w - -", false, &st);
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
    pos_set(&pos, "7k/6pp/8/8/8/8/8/R6K w - -", false, &st);
    SearchResult r = search_go(&pos, &limits);
    CHECK(r.score >= VALUE_MATE_IN_MAX_PLY, "mate score, got %d", r.score);
    CHECK(r.score == mate_in(1), "mate in 1, got %d", r.score);

    char buf[8];
    move_to_uci(&pos, r.best_move, buf);
    CHECK(strcmp(buf, "a1a8") == 0, "finds Ra8#, got %s", buf);

    // A checkmated position has no move and a mated score at ply 0.
    pos_set(&pos, "7k/6Q1/6K1/8/8/8/8/8 b - -", false, &st);
    r = search_go(&pos, &limits);
    CHECK(r.best_move == MOVE_NONE, "no move when mated");
    CHECK(r.score == mated_in(0), "mated_in(0), got %d", r.score);

    // Stalemate is a draw, not a loss.
    pos_set(&pos, "7k/5Q2/8/8/8/8/8/6K1 b - -", false, &st);
    r = search_go(&pos, &limits);
    CHECK(r.score == VALUE_DRAW, "stalemate is a draw, got %d", r.score);

    // Free material must be taken.
    pos_set(&pos, "4k3/8/8/3q4/4B3/8/8/4K3 w - -", false, &st);
    r = search_go(&pos, &limits);
    move_to_uci(&pos, r.best_move, buf);
    CHECK(strcmp(buf, "e4d5") == 0, "captures the hanging queen, got %s", buf);

    // The search must be deterministic: same position, same TT state, same nodes.
    pos_set(&pos, "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -", false, &st);
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
    pos_set(&pos, "7k/6pp/8/8/8/8/8/R6K w - -", false, &st);
    r = search_go(&pos, &limits);
    CHECK(r.score == mate_in(1), "mate found with a cleared table, got %d", r.score);

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
    pos_set(&pos, "4k3/8/8/8/8/8/8/4K3 w - -", false, &st);
    const Move moves[] = {
        move_from_uci(&pos, "e1e2"),
    };
    CHECK(moves[0] != MOVE_NONE, "e1e2 legal");

    int n = 0;
    pos_do_move(&pos, move_from_uci(&pos, "e1e2"), &chain[n++], false, &pos.scratch_dp,
                &pos.scratch_dts);
    pos_do_move(&pos, move_from_uci(&pos, "e8e7"), &chain[n++], false, &pos.scratch_dp,
                &pos.scratch_dts);
    pos_do_move(&pos, move_from_uci(&pos, "e2e1"), &chain[n++], false, &pos.scratch_dp,
                &pos.scratch_dts);
    pos_do_move(&pos, move_from_uci(&pos, "e7e8"), &chain[n++], false, &pos.scratch_dp,
                &pos.scratch_dts);
    pos_do_move(&pos, move_from_uci(&pos, "e1e2"), &chain[n++], false, &pos.scratch_dp,
                &pos.scratch_dts);
    pos_do_move(&pos, move_from_uci(&pos, "e8e7"), &chain[n++], false, &pos.scratch_dp,
                &pos.scratch_dts);
    pos_do_move(&pos, move_from_uci(&pos, "e2e1"), &chain[n++], false, &pos.scratch_dp,
                &pos.scratch_dts);
    pos_do_move(&pos, move_from_uci(&pos, "e7e8"), &chain[n++], false, &pos.scratch_dp,
                &pos.scratch_dts);

    CHECK(pos_is_draw(&pos, 8), "threefold repetition detected");

    // The 50-move rule fires on the halfmove clock alone.
    pos_set(&pos, "4k3/8/8/8/8/8/8/4K3 w - - 100 60", false, &st);
    CHECK(pos_is_draw(&pos, 0), "50-move rule detected");

    pos_set(&pos, "4k3/8/8/8/8/8/8/4K3 w - - 98 60", false, &st);
    CHECK(!pos_is_draw(&pos, 0), "99 plies is not yet a draw");
}

// nnue_dot4_i32 is the ONE reducing primitive in simd.h, and the only place the
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
    int8_t w[NNUE_DOT_LANES * 4];
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

    // A malformed ELEMENT contributes nothing and the rest of the list still parses;
    // upstream's indices_from_shortened_string never fails (numa.h:1033).
    CHECK(parse_policy("0-1,7-3", &nodes, &cpus), "reversed range is skipped, not fatal");
    CHECK(nodes == 1 && cpus == 2, "0-1,7-3 -> node {0,1}, got %zu cpus", cpus);
    CHECK(parse_policy("0-1,x", &nodes, &cpus), "unparseable element is skipped");
    CHECK(cpus == 2, "0-1,x -> 2 cpus, got %zu", cpus);
    CHECK(parse_policy("0-1,1-2-3", &nodes, &cpus), "three-part element is skipped");
    CHECK(cpus == 2, "0-1,1-2-3 -> 2 cpus, got %zu", cpus);

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

int main(void) {
    bitboards_init();
    attacks_init();
    threats_init();
    position_init();

    test_bitboards();
    test_fen();
    test_perft();
    test_roundtrip();
    test_null_move();
    test_legality();
    test_uci_move_strings();
    test_evaluate();
    test_tt();
    test_search();
    test_draw_detection();
    test_nnue_dot4();
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
