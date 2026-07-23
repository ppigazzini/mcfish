// Implement the engine session: the position and its unbounded state chain, the
// search entry points, and the startup sequence that wires the option table
// (engine_options.c) and the net (engine_nnue.c) to the search. engine.h is the
// facade the shell drives; the option and net policy live in their own modules.

#include "engine.h"

#include "../engine/board/board_props.h"
#include "../engine/board/legality.h"
#include "../engine/board/state_list.h"
#include "../engine/board/uci_move.h"
#include "../engine/eval/evaluate.h"
#include "../engine/search/tt.h"
#include "engine_nnue.h"
#include "engine_options.h"
#include "syzygy_option.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENGINE_START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

// Keep the whole state chain alive for the game: pos_undo_move and the repetition
// scan both follow StateInfo::previous, so a state popped off the C stack would leave
// the chain pointing at freed memory. Each record is its own allocation, so a push
// never moves one already handed out. Upstream's chain is a deque (engine.cpp:210).
static Position Pos;
static StateList *States = nullptr;
static char ReasonBuf[128];

void engine_set_output(void (*emit_line)(const char *line),
                       void (*emit_info)(const char *message)) {
    search_set_output(emit_line);
    engine_options_set_info(emit_info);
    engine_nnue_set_output(emit_line);
}

OptionsMap *engine_options(void) { return engine_options_map(); }

Position *engine_position(void) { return &Pos; }

int engine_setoption(const char *args, char name[OPTION_NAME_MAX]) {
    return engine_options_apply(args, name);
}

void engine_render_options(char *buf, size_t buf_len) { engine_options_render(buf, buf_len); }

void engine_report_net(void) { engine_nnue_report(); }

void engine_verify_network(void) { engine_nnue_verify(); }

// ---------------------------------------------------------------------------
// Position
// ---------------------------------------------------------------------------

bool engine_set_position_variant(const char *fen, bool chess960, const char **reason) {
    const char *r = nullptr;
    StateInfo *const root = state_list_reset(States);
    if (pos_set_reason(&Pos, fen, chess960, root, &r))
        return true;
    if (reason)
        *reason = r ? r : "Invalid FEN.";
    return false;
}

bool engine_set_position(const char *fen, const char **reason) {
    return engine_set_position_variant(fen, engine_options_get_int("UCI_Chess960") != 0, reason);
}

bool engine_set_startpos(const char **reason) {
    return engine_set_position(ENGINE_START_FEN, reason);
}

// Re-set the board from the colour-reversed form of its own FEN. The variant comes
// from the BOARD, not the live option: upstream ends Position::flip with
// set(f, is_chess960(), st) (position.cpp:1633).
void engine_flip(const char **reason) {
    char fen[128];
    pos_fen(&Pos, fen);

    char flipped[128];
    if (!pos_flip_fen(fen, flipped))
        return;

    (void) engine_set_position_variant(flipped, board_is_chess960(&Pos), reason);
}

bool engine_play_move(const char *uci_move, const char **reason) {
    const Move m = move_from_uci(&Pos, uci_move);
    if (m == MOVE_NONE) {
        snprintf(ReasonBuf, sizeof ReasonBuf, "Illegal move: %s", uci_move);
        if (reason)
            *reason = ReasonBuf;
        return false;
    }
    StateInfo *const st = state_list_push(States);
    if (st == nullptr) {
        if (reason)
            *reason = "Out of memory extending the state chain.";
        return false;
    }
    pos_do_move(&Pos, m, st, pos_gives_check(&Pos, m), &Pos.scratch_dp, &Pos.scratch_dts);
    return true;
}

void engine_new_game(void) {
    tt_clear();
    search_clear();
    const char *r = nullptr;
    (void) engine_set_startpos(&r);
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

void engine_go(const SearchLimits *limits) {
    // The search zone emits `bestmove` itself, through the installed sink, so the
    // line is built once and in one place. Do not print a second one.
    (void) search_go(&Pos, limits);
}

uint64_t engine_perft(int depth) { return perft(&Pos, depth, true); }

void engine_stop(void) { search_stop(); }

void engine_current_fen(char *buf, size_t buf_len) {
    char fen[128];
    pos_fen(&Pos, fen);
    snprintf(buf, buf_len, "%s", fen);
}

void engine_visualize(char *buf, int buf_len) { pos_pretty(&Pos, buf, buf_len); }

void engine_trace_eval(char *buf, int buf_len) { evaluate_trace(&Pos, buf, buf_len); }

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

void engine_init(const char *argv0) {
    // Build the state chain before anything can set a position.
    States = state_list_create();
    if (States == nullptr) {
        fprintf(stderr, "Out of memory allocating the state chain\n");
        exit(EXIT_FAILURE);
    }

    // Bind the tablebase seams before the first search: until this runs the engine
    // reads the neutral defaults, which never probe.
    syzygy_option_install();

    engine_options_register();

    // Clear the search state before the first command, as upstream does from the
    // Engine constructor (engine.cpp:145): the histories are NOT zero when clear, so
    // an engine that skips this searches a different tree until the first ucinewgame.
    search_clear();

    // Point the search at the option table so MultiPV, Skill Level, UCI_Elo, Move
    // Overhead, nodestime, Ponder and UCI_ShowWDL are read where the handshake
    // advertises them.
    search_set_option_source(engine_options_get_int);

    // Size the table from the registered default rather than a second literal.
    tt_resize((size_t) engine_options_get_int("Hash"));

    const char *r = nullptr;
    (void) engine_set_startpos(&r);

    // Load the net before the first command: go/perft/eval all report the outcome.
    engine_nnue_set_root(argv0);
    engine_nnue_reload(engine_options_get_string("EvalFile"));
}

void engine_shutdown(void) { tt_free(); }
