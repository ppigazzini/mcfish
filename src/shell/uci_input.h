// Read UCI command lines. Own the input stream and its line buffer and nothing
// else — no dispatch, no engine state — so the command loop uses this without a
// cycle back into itself.
//
// The invariant is one call, one command: the returned span never contains a
// '\n', and the trailing "\r\n" a Windows GUI sends is stripped along with the
// bare "\n" every other one sends. The span points into the UciInput the caller
// owns and is valid until the next read.
//
// Golden: upstream `uci.cpp:87` (`std::getline(std::cin, cmd)` in
// UCIEngine::loop).

#ifndef MCFISH_UCI_INPUT_H
#define MCFISH_UCI_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// Bound one command line. A `position startpos moves ...` line grows by five
// bytes a ply, so this holds a game far longer than any that can be played.
enum { UCI_LINE_MAX = 65536 };

typedef struct {
    FILE *stream;
    char line[UCI_LINE_MAX];
    size_t len;
    // Record that the last read outran the buffer and the tail was discarded.
    // The command is still dispatched — a truncated `position` is rejected by
    // the move parser, which is a better answer than treating overflow as EOF
    // and quitting mid-game.
    bool truncated;
} UciInput;

// Point IN at STREAM, or at stdin when STREAM is null.
void uci_input_init(UciInput *in, FILE *stream);

// Read the next line, without its terminator. Return a pointer to it and write
// its length to *LEN_OUT, or return nullptr at end of input. A final line with
// no terminator is returned before the null; an empty line is returned as an
// empty span, not as end of input.
const char *uci_input_read_line(UciInput *in, size_t *len_out);

#endif  // MCFISH_UCI_INPUT_H
