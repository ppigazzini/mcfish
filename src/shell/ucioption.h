// Own the typed UCI option table: every option's kind, default, bounds, current
// value, registration order, and on-change callback.
//
// Registration order IS the wire order. A GUI parses the `uci` handshake in the
// order the engine emits it, and `tools/handshake.golden` diffs it byte for byte,
// so `options_add` appends and `options_render` walks that same sequence — never
// a sort, never a hash order. Storage is fixed: no allocation, bounded names and
// values, and an add past OPTION_MAX is dropped rather than silently overwriting.
//
// Golden: upstream `ucioption.cpp:73` (Option::add),
// `ucioption.cpp:152` (Option::operator=), `ucioption.cpp:186` (operator<<).
//
// Upstream: ucioption.h:36 (class Option).

#ifndef MCFISH_UCIOPTION_H
#define MCFISH_UCIOPTION_H

#include <stddef.h>
#include <stdint.h>

enum {
    OPTION_NAME_MAX = 32,    // longest standard name is "UCI_LimitStrength" (17)
    OPTION_VALUE_MAX = 512,  // SyzygyPath is a path list; EvalFile is a path
    OPTION_MAX = 32,         // the standard set is 19; leave room for Tune options
    OPTIONS_RENDER_MAX = 8192,
};

// Mirror upstream's `type` strings. The order is the enum's own; nothing on the
// wire depends on it, only on option_kind_name.
typedef enum : uint8_t {
    OPTION_STRING = 0,
    OPTION_CHECK = 1,
    OPTION_SPIN = 2,
    OPTION_BUTTON = 3,
    OPTION_COMBO = 4,
} OptionKind;

typedef struct UciOption UciOption;

// Fire after a value is accepted and installed, exactly as upstream's OnChange
// does. Return a message for the info listener, or nullptr for silence. The
// returned pointer must outlive the call — a string literal or a static buffer.
typedef const char *(*OptionOnChange)(const UciOption *opt);

// Receive an on-change message. Upstream routes this through OptionsMap::info;
// here it is what lets the option table stay free of stdio.
typedef void (*OptionInfoFn)(const char *message);

struct UciOption {
    char name[OPTION_NAME_MAX];
    char default_value[OPTION_VALUE_MAX];
    char current_value[OPTION_VALUE_MAX];
    int min;
    int max;
    OptionKind kind;
    OptionOnChange on_change;
};

typedef struct {
    UciOption entries[OPTION_MAX];
    int count;
    OptionInfoFn info;
} OptionsMap;

typedef enum : uint8_t {
    OPTION_SET_OK = 0,        // found, accepted, installed, callback fired
    OPTION_SET_UNKNOWN = 1,   // no option by that name
    OPTION_SET_REJECTED = 2,  // found, but the value failed its kind's validation
} OptionSetResult;

const char *option_kind_name(OptionKind kind);

// Compare two option names the way upstream's CaseInsensitiveLess does — every
// lookup on the wire is case-insensitive. Port of upstream `ucioption.cpp:33`.
bool option_name_equals(const char *a, const char *b);

// Empty the table. Call before registering; there is no free to pair with it.
void options_clear(OptionsMap *map);

// Install the message sink the on-change callbacks report through.
void options_set_info(OptionsMap *map, OptionInfoFn info);

// Append one option in wire order. DEFAULT_VALUE is the literal default text:
// "true"/"false" for check, the decimal digits for spin, the `var`-free
// space-separated choice list for combo, "" for button. Return the registration
// index, or -1 when the table is full or the name is already present — upstream
// exits the process on a duplicate (ucioption.cpp:83); here the caller decides.
int options_add(OptionsMap *map,
                const char *name,
                OptionKind kind,
                const char *default_value,
                int min,
                int max,
                OptionOnChange on_change);

// Look up by name, case-insensitively. Return nullptr when absent.
const UciOption *options_find(const OptionsMap *map, const char *name);

// Assign a value, validating against the option's kind and bounds exactly as
// upstream `Option::operator=` does, then fire on_change and route its message
// to the info listener. A rejected value leaves the current value untouched.
OptionSetResult options_set(OptionsMap *map, const char *name, const char *value);

// Parse and apply one `setoption` command body — everything after the command
// word, i.e. `name <id...> [value <val...>]`. Both fields may contain spaces and
// are re-joined with single spaces. Port of upstream `ucioption.cpp:44`.
// Write the parsed name into NAME_OUT (size OPTION_NAME_MAX) so the caller can
// report `No such option: <name>`; pass nullptr to discard it.
OptionSetResult options_setoption(OptionsMap *map, const char *args, char *name_out);

// Read a spin as its integer or a check as 0/1, matching upstream's
// `Option::operator int()`. Return 0 for any other kind and for an absent name.
int options_get_int(const OptionsMap *map, const char *name);

// Read the current value as text. Return "" when absent, never nullptr.
const char *options_get_string(const OptionsMap *map, const char *name);

// Render the handshake option block into BUF. Each line is prefixed with '\n'
// and the block carries no trailing newline, so it drops between `id author ...`
// and `\nuciok` the way upstream's `operator<<` composes in uci.cpp. Return the
// number of bytes written, excluding the NUL; truncate rather than overrun.
size_t options_render(const OptionsMap *map, char *buf, size_t buf_len);

#endif  // MCFISH_UCIOPTION_H
