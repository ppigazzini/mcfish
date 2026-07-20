#include "ucioption.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Name comparison
// ---------------------------------------------------------------------------

static char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char) (c + ('a' - 'A')) : c; }

// Port of upstream CaseInsensitiveLess (ucioption.cpp:33): a lexicographic
// compare over tolower'd bytes, with the shorter string ordering first on a
// common prefix.
static bool name_less(const char *a, const char *b) {
    while (*a && *b) {
        const char la = ascii_lower(*a);
        const char lb = ascii_lower(*b);
        if (la != lb)
            return la < lb;
        ++a;
        ++b;
    }
    return *a == '\0' && *b != '\0';
}

bool option_name_equals(const char *a, const char *b) {
    return !name_less(a, b) && !name_less(b, a);
}

const char *option_kind_name(OptionKind kind) {
    switch (kind) {
    case OPTION_STRING :
        return "string";
    case OPTION_CHECK :
        return "check";
    case OPTION_SPIN :
        return "spin";
    case OPTION_BUTTON :
        return "button";
    case OPTION_COMBO :
        return "combo";
    }
    return "string";
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

// Copy at most CAP-1 bytes and always NUL-terminate. Every string that enters
// the table goes through here, which is what makes the fixed arrays safe.
static void store(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    while (src[n] && n + 1 < cap)
        ++n;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void options_clear(OptionsMap *map) {
    map->count = 0;
    map->info = nullptr;
}

void options_set_info(OptionsMap *map, OptionInfoFn info) { map->info = info; }

static UciOption *find_mut(OptionsMap *map, const char *name) {
    for (int i = 0; i < map->count; ++i)
        if (option_name_equals(map->entries[i].name, name))
            return &map->entries[i];
    return nullptr;
}

const UciOption *options_find(const OptionsMap *map, const char *name) {
    for (int i = 0; i < map->count; ++i)
        if (option_name_equals(map->entries[i].name, name))
            return &map->entries[i];
    return nullptr;
}

int options_add(OptionsMap *map,
                const char *name,
                OptionKind kind,
                const char *default_value,
                int min,
                int max,
                OptionOnChange on_change) {
    if (map->count >= OPTION_MAX || find_mut(map, name))
        return -1;

    UciOption *o = &map->entries[map->count];
    store(o->name, sizeof o->name, name);
    store(o->default_value, sizeof o->default_value, default_value);
    // A combo's currentValue starts at its default like every other kind here;
    // upstream's two-argument combo constructor (ucioption.cpp:122) can start it
    // elsewhere, and no standard option uses that form.
    store(o->current_value, sizeof o->current_value, default_value);
    o->kind = kind;
    o->min = min;
    o->max = max;
    o->on_change = on_change;

    return map->count++;
}

// ---------------------------------------------------------------------------
// Validation — upstream Option::operator= (ucioption.cpp:162)
// ---------------------------------------------------------------------------

// Port of upstream value_in_range (ucioption.cpp:148). Reject an empty string, a
// value with trailing non-digits, and an out-of-range magnitude, so that
// `setoption name Hash value 12x` is refused rather than silently read as 12.
// Parse as long long and compare against the int bounds: strtol on a 32-bit long
// would saturate a huge value to LONG_MAX and could land it inside the range.
static bool value_in_range(const char *v, int min, int max) {
    if (v[0] == '\0')
        return false;
    errno = 0;
    char *end = nullptr;
    const long long result = strtoll(v, &end, 10);
    if (errno == ERANGE || *end != '\0')
        return false;
    return result >= min && result <= max;
}

// Report whether VALUE appears among the space-separated choices in CHOICES.
static bool combo_contains(const char *choices, const char *value) {
    const char *p = choices;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            ++p;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t')
            ++p;
        const size_t len = (size_t) (p - start);
        if (len == 0)
            continue;
        char token[OPTION_VALUE_MAX];
        const size_t n = len < sizeof token - 1 ? len : sizeof token - 1;
        memcpy(token, start, n);
        token[n] = '\0';
        if (option_name_equals(token, value))
            return true;
    }
    return false;
}

static bool accepts(const UciOption *o, const char *value) {
    // An empty value is meaningful only for a button (which ignores it) and for
    // a string (which may legitimately be cleared).
    if (o->kind != OPTION_BUTTON && o->kind != OPTION_STRING && value[0] == '\0')
        return false;

    switch (o->kind) {
    case OPTION_CHECK :
        return strcmp(value, "true") == 0 || strcmp(value, "false") == 0;
    case OPTION_SPIN :
        return value_in_range(value, o->min, o->max);
    case OPTION_COMBO :
        // `var` is the grammar's separator keyword, never a choice.
        return strcmp(value, "var") != 0 && combo_contains(o->default_value, value);
    case OPTION_STRING :
    case OPTION_BUTTON :
        return true;
    }
    return false;
}

OptionSetResult options_set(OptionsMap *map, const char *name, const char *value) {
    UciOption *o = find_mut(map, name);
    if (!o)
        return OPTION_SET_UNKNOWN;

    if (!accepts(o, value))
        return OPTION_SET_REJECTED;

    if (o->kind == OPTION_STRING)
        store(o->current_value, sizeof o->current_value,
              strcmp(value, "<empty>") == 0 ? "" : value);
    else if (o->kind != OPTION_BUTTON)
        store(o->current_value, sizeof o->current_value, value);

    // Fire on every accepted assignment, not only on a changed one: upstream
    // does, and `setoption name Clear Hash` depends on it (a button never
    // changes a value, and re-setting Hash to its current size still resizes).
    if (o->on_change) {
        const char *message = o->on_change(o);
        if (message && map->info)
            map->info(message);
    }

    return OPTION_SET_OK;
}

// ---------------------------------------------------------------------------
// setoption parsing — upstream OptionsMap::setoption (ucioption.cpp:44)
// ---------------------------------------------------------------------------

// Append TOKEN to FIELD, separated from any existing content by one space, so
// that a multi-word name or value is re-joined with single-space canonical
// spacing regardless of the whitespace the GUI sent.
static void append_token(char *field, size_t cap, const char *token, size_t token_len) {
    size_t n = strlen(field);
    if (n && n + 1 < cap)
        field[n++] = ' ';
    const size_t room = cap > n + 1 ? cap - n - 1 : 0;
    const size_t take = token_len < room ? token_len : room;
    memcpy(field + n, token, take);
    field[n + take] = '\0';
}

OptionSetResult options_setoption(OptionsMap *map, const char *args, char *name_out) {
    char name[OPTION_NAME_MAX] = { 0 };
    char value[OPTION_VALUE_MAX] = { 0 };

    const char *p = args;
    bool in_value = false;
    bool first = true;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            ++p;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
            ++p;
        const size_t len = (size_t) (p - start);

        // Drop the leading `name` keyword unconditionally, as upstream's first
        // `is >> token` does — it is consumed before the name loop begins.
        if (first) {
            first = false;
            continue;
        }
        if (!in_value && len == 5 && memcmp(start, "value", 5) == 0) {
            in_value = true;
            continue;
        }
        if (in_value)
            append_token(value, sizeof value, start, len);
        else
            append_token(name, sizeof name, start, len);
    }

    if (name_out)
        store(name_out, OPTION_NAME_MAX, name);

    return options_set(map, name, value);
}

// ---------------------------------------------------------------------------
// Typed reads
// ---------------------------------------------------------------------------

int options_get_int(const OptionsMap *map, const char *name) {
    const UciOption *o = options_find(map, name);
    if (!o)
        return 0;
    if (o->kind == OPTION_SPIN)
        return (int) strtol(o->current_value, nullptr, 10);
    if (o->kind == OPTION_CHECK)
        return strcmp(o->current_value, "true") == 0 ? 1 : 0;
    return 0;
}

const char *options_get_string(const OptionsMap *map, const char *name) {
    const UciOption *o = options_find(map, name);
    return o ? o->current_value : "";
}

// ---------------------------------------------------------------------------
// Rendering — upstream operator<<(ostream&, const OptionsMap&) (ucioption.cpp:198)
// ---------------------------------------------------------------------------

// Append at most what fits, and keep *POS advancing past the end so the caller
// still learns the truncation happened by comparing against buf_len.
[[gnu::format(printf, 4, 5)]]
static void render_append(char *buf, size_t buf_len, size_t *pos, const char *fmt, ...) {
    if (*pos >= buf_len)
        return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + *pos, buf_len - *pos, fmt, ap);
    va_end(ap);
    if (n > 0)
        *pos += (size_t) n;
}

size_t options_render(const OptionsMap *map, char *buf, size_t buf_len) {
    if (!buf_len)
        return 0;
    buf[0] = '\0';

    size_t pos = 0;
    for (int i = 0; i < map->count; ++i) {
        const UciOption *o = &map->entries[i];
        render_append(buf, buf_len, &pos, "\noption name %s type %s", o->name,
                      option_kind_name(o->kind));

        switch (o->kind) {
        case OPTION_CHECK :
        case OPTION_COMBO :
            render_append(buf, buf_len, &pos, " default %s", o->default_value);
            break;
        case OPTION_STRING :
            // An empty default is spelled `<empty>` on the wire; the parse side
            // maps it back to "" in options_set.
            render_append(buf, buf_len, &pos, " default %s",
                          o->default_value[0] ? o->default_value : "<empty>");
            break;
        case OPTION_SPIN :
            // Re-parse the default rather than echoing the stored text, as
            // upstream's `stoi(o.defaultValue)` does, so a default written with
            // leading zeros or a sign renders canonically.
            render_append(buf, buf_len, &pos, " default %d min %d max %d",
                          (int) strtol(o->default_value, nullptr, 10), o->min, o->max);
            break;
        case OPTION_BUTTON :
            break;
        }
    }

    return pos < buf_len ? pos : buf_len - 1;
}
