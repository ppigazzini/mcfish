#include "uci_input.h"

void uci_input_init(UciInput *in, FILE *stream) {
    in->stream = stream ? stream : stdin;
    in->line[0] = '\0';
    in->len = 0;
    in->truncated = false;
}

const char *uci_input_read_line(UciInput *in, size_t *len_out) {
    FILE *stream = in->stream ? in->stream : stdin;

    size_t len = 0;
    bool saw_any = false;
    bool overflow = false;

    for (;;) {
        const int c = fgetc(stream);
        if (c == EOF)
            break;
        saw_any = true;
        if (c == '\n')
            break;

        // Keep consuming past the bound so the next read starts at the next
        // line rather than mid-command.
        if (len + 1 < sizeof in->line)
            in->line[len++] = (char) c;
        else
            overflow = true;
    }

    if (!saw_any) {
        in->len = 0;
        in->truncated = false;
        return nullptr;  // end of input
    }

    // Drop the carriage return a Windows GUI leaves before the newline. Strip
    // every trailing CR/LF, not just one, so a doubled terminator cannot leave a
    // stray byte on the end of the last token.
    while (len > 0 && (in->line[len - 1] == '\r' || in->line[len - 1] == '\n'))
        --len;

    in->line[len] = '\0';
    in->len = len;
    in->truncated = overflow;

    if (len_out)
        *len_out = len;
    return in->line;
}
