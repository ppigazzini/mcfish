#include "registry.h"

#include "decode.h"
#include "encode.h"
#include "tables.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Index piece types as upstream does: 1 = pawn .. 6 = king, black = 8 | type.
enum : uint8_t { PT_PAWN = 1, PT_KING = 6 };

static const char PieceChar[] = " PNBRQK";
static const uint8_t WdlMagic[4] = { 0x71, 0xE8, 0x23, 0x5D };
static const uint8_t DtzMagic[4] = { 0xD7, 0x66, 0x0C, 0xA5 };
static const char SepChar = ':';  // Linux is the only supported target

enum : size_t { HASH_SIZE = 1 << 12, HASH_MASK = HASH_SIZE - 1 };  // upstream TBTables::Size

static uint64_t HashKeys[HASH_SIZE];
static TBTable *HashTabs[HASH_SIZE];

static size_t FoundWdl = 0;
static size_t FoundDtz = 0;
static size_t MaxCard = 0;
static bool Registry_ready = false;
static bool GeometryReady = false;

static char PathBuf[4096];
static size_t PathLen = 0;

// Track every allocation and every mapping so `registry_init` can release the
// previous generation wholesale — a TBTable's pointers all die together.
typedef struct Chunk {
    void *ptr;
    size_t size;  // non-zero for a mapping, zero for a malloc'd block
    struct Chunk *next;
} Chunk;

static Chunk *Chunks = nullptr;

static void *registry_alloc(size_t bytes) {
    if (bytes == 0) {
        bytes = 1;
    }
    void *p = malloc(bytes);
    if (p == nullptr) {
        return nullptr;
    }
    Chunk *c = malloc(sizeof(Chunk));
    if (c == nullptr) {
        free(p);
        return nullptr;
    }
    c->ptr = p;
    c->size = 0;
    c->next = Chunks;
    Chunks = c;
    return p;
}

static bool record_mapping(void *addr, size_t size) {
    Chunk *c = malloc(sizeof(Chunk));
    if (c == nullptr) {
        return false;
    }
    c->ptr = addr;
    c->size = size;
    c->next = Chunks;
    Chunks = c;
    return true;
}

static void release_all(void) {
    Chunk *c = Chunks;
    while (c != nullptr) {
        Chunk *next = c->next;
        if (c->size != 0) {
            munmap(c->ptr, c->size);
        } else {
            free(c->ptr);
        }
        free(c);
        c = next;
    }
    Chunks = nullptr;
}

// ---- material key -----------------------------------------------------------

// Hash the material configuration. Mirror upstream's `Position::material_key`
// shape — one key per (piece, ordinal) pair, XORed — with a table private to this
// module, because mcfish's Position carries no material key. Only agreement
// between a registered table and a probed position matters.
static uint64_t MaterialKeys[16][17];
static bool MaterialKeysReady = false;

static void init_material_keys(void) {
    uint64_t s = 1070372;  // fixed seed: the registry must not vary run to run
    for (size_t pc = 0; pc < 16; ++pc) {
        for (size_t i = 0; i < 17; ++i) {
            s ^= s >> 12;
            s ^= s << 25;
            s ^= s >> 27;
            MaterialKeys[pc][i] = s * 2685821657736338717ULL;
        }
    }
    MaterialKeysReady = true;
}

uint64_t syzygy_material_key(const int counts[16]) {
    if (!MaterialKeysReady) {
        init_material_keys();
    }
    uint64_t key = 0;
    for (size_t pc = 0; pc < 16; ++pc) {
        const int n = counts[pc] > 16 ? 16 : counts[pc];
        for (int i = 0; i < n; ++i) {
            key ^= MaterialKeys[pc][i];
        }
    }
    return key;
}

// ---- registry ---------------------------------------------------------------

bool registry_ready(void) { return Registry_ready; }
size_t registry_max_cardinality(void) { return MaxCard; }
size_t registry_discovered_max(void) { return MaxCard; }
size_t registry_found_wdl(void) { return FoundWdl; }
size_t registry_found_dtz(void) { return FoundDtz; }

static void hash_insert(uint64_t key, TBTable *t) {
    size_t i = (size_t) key & HASH_MASK;
    size_t probes = 0;
    while (HashTabs[i] != nullptr) {
        i = (i + 1) & HASH_MASK;
        if (++probes >= HASH_SIZE) {
            return;  // full: refuse rather than spin
        }
    }
    HashKeys[i] = key;
    HashTabs[i] = t;
}

TBTable *registry_get(uint64_t key) {
    size_t i = (size_t) key & HASH_MASK;
    size_t probes = 0;
    while (HashTabs[i] != nullptr) {
        if (HashKeys[i] == key) {
            return HashTabs[i];
        }
        i = (i + 1) & HASH_MASK;
        if (++probes >= HASH_SIZE) {
            break;
        }
    }
    return nullptr;
}

PairsData *tbtable_get(TBTable *t, bool dtz, size_t stm, size_t f) {
    const size_t file = t->has_pawns ? f : 0;
    if (dtz) {
        return &t->dtz_items[0][file];
    }
    return &t->items[stm % t->sides][file];
}

// Build the canonical stem: one char per piece, with 'v' inserted before the
// second 'K'. {K,Q,K} becomes "KQvK"; {K,R,P,K,R} becomes "KRPvKR".
static void build_stem(const uint8_t *pieces, size_t n_pieces, char *out, size_t *out_len) {
    size_t n = 0;
    for (size_t i = 0; i < n_pieces && n < 7; ++i) {
        out[n++] = PieceChar[pieces[i]];
    }
    size_t k = 1;
    while (k < n && out[k] != 'K') {
        ++k;
    }
    for (size_t j = n; j > k; --j) {
        out[j] = out[j - 1];
    }
    out[k] = 'v';
    *out_len = n + 1;
}

// Register a WDL table for PIECES (e.g. {K,Q,K}): compute both material keys and
// the pawn/unique-piece flags upstream derives from a code position, then insert
// under key and key2.
static void registry_register(const uint8_t *pieces, size_t n_pieces) {
    // Split the code at the second king: white (strong) is [0, k2), black is [k2, n).
    size_t k2 = 1;
    while (k2 < n_pieces && pieces[k2] != PT_KING) {
        ++k2;
    }

    int counts[16] = { };
    for (size_t i = 0; i < k2; ++i) {
        counts[pieces[i]] += 1;  // white piece code == type
    }
    for (size_t i = k2; i < n_pieces; ++i) {
        counts[pieces[i] | 8u] += 1;  // black piece code == 8 | type
    }

    const uint64_t key = syzygy_material_key(counts);
    int counts2[16] = { };
    for (size_t i = 0; i < 16; ++i) {
        counts2[i ^ 8u] = counts[i];  // color swap
    }
    const uint64_t key2 = syzygy_material_key(counts2);

    const int wp = counts[PT_PAWN];
    const int bp = counts[PT_PAWN | 8];
    const bool has_pawns = wp != 0 || bp != 0;
    bool has_unique = false;
    for (size_t pt = PT_PAWN; pt < PT_KING; ++pt) {
        if (counts[pt] == 1 || counts[pt | 8u] == 1) {
            has_unique = true;
        }
    }

    // Lead with WHITE unless both sides have pawns and black has fewer, which
    // compresses better.
    const bool lead_white = (bp == 0) || (wp != 0 && bp >= wp);

    TBTable *t = registry_alloc(sizeof(TBTable));
    if (t == nullptr) {
        return;
    }
    memset(t, 0, sizeof *t);
    // Initialise the two publication flags explicitly rather than leaning on the
    // memset: a zeroed atomic is a valid `false` on every platform mcfish builds
    // for, but only atomic_init makes that a guarantee rather than an observation.
    atomic_bool_init(&t->ready, false);
    atomic_bool_init(&t->dtz_ready, false);
    t->key = key;
    t->key2 = key2;
    t->piece_count = (int32_t) n_pieces;
    t->has_pawns = has_pawns;
    t->has_unique_pieces = has_unique;
    t->pawn_count[0] = (uint8_t) (lead_white ? wp : bp);
    t->pawn_count[1] = (uint8_t) (lead_white ? bp : wp);
    t->sides = (key != key2) ? 2 : 1;
    build_stem(pieces, n_pieces, t->stem, &t->stem_len);

    hash_insert(key, t);
    if (key2 != key) {
        hash_insert(key2, t);
    }
}

// ---- file discovery and mapping ---------------------------------------------

// Write "<dir>/<stem><ext>" into OUT. Return false when it does not fit.
static bool join_path(char *out,
                      size_t out_size,
                      const char *dir,
                      size_t dir_len,
                      const char *stem,
                      size_t stem_len,
                      const char *ext) {
    const size_t ext_len = strlen(ext);
    if (dir_len + 1 + stem_len + ext_len + 1 > out_size) {
        return false;
    }
    memcpy(out, dir, dir_len);
    out[dir_len] = '/';
    memcpy(out + dir_len + 1, stem, stem_len);
    memcpy(out + dir_len + 1 + stem_len, ext, ext_len);
    out[dir_len + 1 + stem_len + ext_len] = '\0';
    return true;
}

// Call BODY-style iteration over the colon-separated SyzygyPath entries.
typedef bool (*PathVisitFn)(const char *dir, size_t dir_len, void *ctx);

static bool for_each_path_dir(PathVisitFn visit, void *ctx) {
    size_t i = 0;
    while (i < PathLen) {
        size_t j = i;
        while (j < PathLen && PathBuf[j] != SepChar) {
            ++j;
        }
        if (j > i && visit(PathBuf + i, j - i, ctx)) {
            return true;
        }
        i = j + 1;
    }
    return false;
}

typedef struct {
    const char *stem;
    size_t stem_len;
    const char *ext;
} ExistsCtx;

static bool visit_exists(const char *dir, size_t dir_len, void *ctx) {
    const ExistsCtx *c = ctx;
    char full[4200];
    if (!join_path(full, sizeof full, dir, dir_len, c->stem, c->stem_len, c->ext)) {
        return false;
    }
    return access(full, F_OK) == 0;
}

static bool tb_file_exists(const char *stem, size_t stem_len, const char *ext) {
    ExistsCtx c = { .stem = stem, .stem_len = stem_len, .ext = ext };
    return for_each_path_dir(visit_exists, &c);
}

typedef struct {
    const char *stem;
    size_t stem_len;
    const char *ext;
    const uint8_t *magic;
    const uint8_t *base;
    size_t size;
} MapCtx;

// Map <dir>/<stem><ext> read-only and verify its magic and its length. Return
// false — leaving the context empty — on any failure, so the caller keeps looking.
static bool visit_map(const char *dir, size_t dir_len, void *ctx) {
    MapCtx *c = ctx;
    char full[4200];
    if (!join_path(full, sizeof full, dir, dir_len, c->stem, c->stem_len, c->ext)) {
        return false;
    }
    const int fd = open(full, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    struct stat sb;
    if (fstat(fd, &sb) != 0 || sb.st_size <= 0) {
        close(fd);
        return false;
    }
    const size_t size = (size_t) sb.st_size;
    // Refuse a file whose length is not the shape upstream asserts: the data
    // sections are 64-byte aligned from a 16-byte header.
    //
    // DELIBERATE DEVIATION from upstream (syzygy/tbprobe.cpp:267-271), which
    // prints `Corrupt tablebase file` and `exit(EXIT_FAILURE)`s. Killing the
    // process for one bad file loses a GUI its engine mid-game, and the rest of
    // the set is still usable, so this reports the file unavailable and plays on —
    // the same fail-soft choice mcfish makes for a net that will not load. Keep
    // the diagnostic: without it a corrupt file is indistinguishable from an
    // absent one, and the engine silently stops probing with nothing to explain it.
    if (size < 8 || size % 64 != 16) {
        fprintf(stderr, "info string Corrupt tablebase file %s\n", full);
        close(fd);
        return false;
    }
    void *addr = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (addr == MAP_FAILED) {
        return false;
    }
    if (memcmp(addr, c->magic, 4) != 0 || !record_mapping(addr, size)) {
        munmap(addr, size);
        return false;
    }
    c->base = addr;
    c->size = size;
    return true;
}

static const uint8_t *
map_file(const TBTable *t, const char *ext, const uint8_t magic[4], size_t *out_size) {
    MapCtx c = { .stem = t->stem,
                 .stem_len = t->stem_len,
                 .ext = ext,
                 .magic = magic,
                 .base = nullptr,
                 .size = 0 };
    if (!for_each_path_dir(visit_map, &c)) {
        return nullptr;
    }
    *out_size = c.size;
    return c.base;
}

// ---- set: parse the file's PairsData records --------------------------------

static bool need(size_t pos, size_t bytes, size_t len) {
    return bytes <= len && pos <= len - bytes;
}

// Port upstream `set_dtz_map` (syzygy/tbprobe.cpp:1158): read the per-file DTZ
// value-remap tables. map_idx[i] records each WDL-class map's offset from
// dtz_map — in uint16 units when the table is wide, in bytes otherwise, +1 as
// upstream.
static bool
set_dtz_map(TBTable *t, const uint8_t *buf, size_t buf_len, size_t *pos, size_t max_file) {
    t->dtz_map = buf + *pos;
    t->dtz_map_size = buf_len - *pos;
    const size_t map_base = *pos;

    for (size_t f = 0; f <= max_file; ++f) {
        PairsData *d = tbtable_get(t, true, 0, f);
        if ((d->flags & TB_FLAG_MAPPED) == 0) {
            continue;
        }
        if (d->flags & TB_FLAG_WIDE) {
            *pos += *pos & 1;  // word align
            for (size_t i = 0; i < 4; ++i) {
                if (!need(*pos, 2, buf_len)) {
                    return false;
                }
                d->map_idx[i] = (uint16_t) ((*pos - map_base) / 2 + 1);
                *pos += 2 * (size_t) rd_u16le(buf + *pos) + 2;
            }
        } else {
            for (size_t i = 0; i < 4; ++i) {
                if (!need(*pos, 1, buf_len)) {
                    return false;
                }
                d->map_idx[i] = (uint16_t) (*pos - map_base + 1);
                *pos += (size_t) buf[*pos] + 1;
            }
        }
        if (*pos > buf_len) {
            return false;
        }
    }
    *pos += *pos & 1;  // word align
    return *pos <= buf_len;
}

// Port upstream `set` (syzygy/tbprobe.cpp:1193), one function over WDL and DTZ.
// BUF is the whole mapped file; parsing starts after the 4-byte magic. Return
// false on a truncated or corrupt file, which leaves the table unusable.
static bool set(TBTable *t, bool dtz, const uint8_t *buf, size_t buf_len) {
    const EntryInfo e = { .has_pawns = t->has_pawns,
                          .has_unique_pieces = t->has_unique_pieces,
                          .piece_count = t->piece_count,
                          .pawn_count = { t->pawn_count[0], t->pawn_count[1] } };
    size_t pos = 4;  // skip the magic
    pos += 1;        // skip the Split/HasPawns flag byte upstream only asserts on

    // Treat DTZ as one-sided; a split WDL table (key != key2) stores both sides.
    const size_t sides = dtz ? 1 : t->sides;
    const size_t max_file = t->has_pawns ? 3 : 0;  // FILE_D or FILE_A
    const bool pp = t->has_pawns && t->pawn_count[1] != 0;

    for (size_t f = 0; f <= max_file; ++f) {
        for (size_t i = 0; i < sides; ++i) {
            memset(tbtable_get(t, dtz, i, f), 0, sizeof(PairsData));
        }

        if (!need(pos, (size_t) 1 + (pp ? 1u : 0u), buf_len)) {
            return false;
        }
        int32_t order[2][2];
        order[0][0] = (int32_t) (buf[pos] & 0xF);
        order[0][1] = pp ? (int32_t) (buf[pos + 1] & 0xF) : 0xF;
        order[1][0] = (int32_t) (buf[pos] >> 4);
        order[1][1] = pp ? (int32_t) (buf[pos + 1] >> 4) : 0xF;
        pos += (size_t) 1 + (pp ? 1u : 0u);

        for (size_t k = 0; k < (size_t) t->piece_count && k < TB_PIECES; ++k) {
            if (!need(pos, 1, buf_len)) {
                return false;
            }
            for (size_t i = 0; i < sides; ++i) {
                tbtable_get(t, dtz, i, f)->pieces[k] =
                  (uint8_t) (i != 0 ? buf[pos] >> 4 : buf[pos] & 0xF);
            }
            pos += 1;
        }
        for (size_t i = 0; i < sides; ++i) {
            set_groups(tbtable_get(t, dtz, i, f), e, order[i], f);
        }
    }

    pos += pos & 1;  // word alignment: the base is page-aligned, so offset parity is address parity

    for (size_t f = 0; f <= max_file; ++f) {
        for (size_t i = 0; i < sides; ++i) {
            if (!decode_set_sizes(tbtable_get(t, dtz, i, f), buf, buf_len, &pos, registry_alloc)) {
                return false;
            }
        }
    }

    if (dtz && !set_dtz_map(t, buf, buf_len, &pos, max_file)) {
        return false;
    }

    for (size_t f = 0; f <= max_file; ++f) {
        for (size_t i = 0; i < sides; ++i) {
            PairsData *d = tbtable_get(t, dtz, i, f);
            const size_t span = d->sparse_index_size * sizeof(SparseEntry);
            if (!need(pos, span, buf_len)) {
                return false;
            }
            d->sparse_index = buf + pos;
            pos += span;
        }
    }
    for (size_t f = 0; f <= max_file; ++f) {
        for (size_t i = 0; i < sides; ++i) {
            PairsData *d = tbtable_get(t, dtz, i, f);
            const size_t span = (size_t) d->block_length_size * 2;
            if (!need(pos, span, buf_len)) {
                return false;
            }
            d->block_length = buf + pos;
            pos += span;
        }
    }
    for (size_t f = 0; f <= max_file; ++f) {
        for (size_t i = 0; i < sides; ++i) {
            pos = (pos + 0x3F) & ~(size_t) 0x3F;  // 64-byte alignment
            PairsData *d = tbtable_get(t, dtz, i, f);
            const size_t span = (size_t) d->blocks_num * d->sizeof_block;
            if (!need(pos, span, buf_len)) {
                return false;
            }
            d->data = buf + pos;
            d->data_size = span;
            pos += span;
        }
    }
    return true;
}

// Serialise the lazy maps. Upstream's `mapped` declares `static std::mutex mutex`
// inside a function template, so WDL and DTZ each get their own; keep that split
// rather than one lock, or a thread mapping a `.rtbz` blocks one mapping an
// unrelated `.rtbw`. Static-initialised because there is no init hook to run and
// PTHREAD_MUTEX_INITIALIZER is exactly what a `static std::mutex` compiles to.
static Mutex WdlMapMutex = { .handle = PTHREAD_MUTEX_INITIALIZER };
static Mutex DtzMapMutex = { .handle = PTHREAD_MUTEX_INITIALIZER };

// Golden: `Stockfish/src/syzygy/tbprobe.cpp: mapped` (1263-1302), which every
// probing thread may enter at once.
//
// The flag is published LAST, and only after `set` has filled the PairsData. It
// used to be raised on entry, before the file was even opened: a second thread
// then took the fast path and read either a null base -- reporting "no such
// table" for a table that exists -- or a base whose records were still being
// parsed underneath it.
//
// mcfish's AtomicBool is seq_cst where upstream is acquire/release. That is
// strictly stronger, so the guarantee upstream relies on -- that a thread seeing
// `ready` also sees every write made before it was set -- holds; the difference
// costs a fence on a path taken once per table per game.
bool registry_map_wdl(TBTable *t) {
    if (atomic_bool_load(&t->ready))
        return t->base != nullptr;

    mutex_lock(&WdlMapMutex);
    if (atomic_bool_load(&t->ready)) {  // recheck: another thread may have mapped it
        mutex_unlock(&WdlMapMutex);
        return t->base != nullptr;
    }

    size_t size = 0;
    const uint8_t *buf = map_file(t, ".rtbw", WdlMagic, &size);
    if (buf != nullptr && set(t, false, buf, size)) {
        t->base = buf;
        t->base_size = size;
    } else {
        t->base = nullptr;  // a mapping made here stays owned by the chunk list
    }

    const bool mapped = t->base != nullptr;
    atomic_bool_store(&t->ready, true);
    mutex_unlock(&WdlMapMutex);
    return mapped;
}

bool registry_map_dtz(TBTable *t) {
    if (atomic_bool_load(&t->dtz_ready))
        return t->dtz_base != nullptr;

    mutex_lock(&DtzMapMutex);
    if (atomic_bool_load(&t->dtz_ready)) {
        mutex_unlock(&DtzMapMutex);
        return t->dtz_base != nullptr;
    }

    size_t size = 0;
    const uint8_t *buf = map_file(t, ".rtbz", DtzMagic, &size);
    if (buf != nullptr && set(t, true, buf, size)) {
        t->dtz_base = buf;
        t->dtz_base_size = size;
    } else {
        t->dtz_base = nullptr;
    }

    const bool mapped = t->dtz_base != nullptr;
    atomic_bool_store(&t->dtz_ready, true);
    mutex_unlock(&DtzMapMutex);
    return mapped;
}

// ---- init: enumerate every material configuration ---------------------------

// Port upstream `TBTables::add`: count the DTZ file when present, then the WDL
// file — a table counts as found only when its `.rtbw` exists — and raise the max
// cardinality to this configuration's piece count.
static void add(const uint8_t *pieces, size_t n) {
    char stem[8];
    size_t stem_len = 0;
    build_stem(pieces, n, stem, &stem_len);
    if (tb_file_exists(stem, stem_len, ".rtbz")) {
        FoundDtz += 1;
    }
    if (!tb_file_exists(stem, stem_len, ".rtbw")) {
        return;
    }
    FoundWdl += 1;
    if (n > MaxCard) {
        MaxCard = n;
    }
    registry_register(pieces, n);
}

#define ADD(...) \
    do { \
        const uint8_t pcs[] = { __VA_ARGS__ }; \
        add(pcs, sizeof pcs); \
    } while (0)

// Port upstream `Tablebases::init` (syzygy/tbprobe.cpp:1397): enumerate every
// material configuration up to 7 men and add each.
void registry_init(const char *path, size_t path_len) {
    release_all();
    memset(HashKeys, 0, sizeof HashKeys);
    memset(HashTabs, 0, sizeof HashTabs);
    FoundWdl = 0;
    FoundDtz = 0;
    MaxCard = 0;
    PathLen = 0;
    Registry_ready = false;

    if (path == nullptr || path_len == 0) {
        return;
    }
    const size_t n = path_len < sizeof PathBuf ? path_len : sizeof PathBuf;
    memcpy(PathBuf, path, n);
    PathLen = n;
    Registry_ready = true;

    if (!GeometryReady) {
        encode_init_geometry();
        GeometryReady = true;
    }

    const uint8_t k = PT_KING;
    for (uint8_t p1 = PT_PAWN; p1 < PT_KING; ++p1) {
        ADD(k, p1, k);
        for (uint8_t p2 = PT_PAWN; p2 <= p1; ++p2) {
            ADD(k, p1, p2, k);
            ADD(k, p1, k, p2);
            for (uint8_t p3 = PT_PAWN; p3 < PT_KING; ++p3) {
                ADD(k, p1, p2, k, p3);
            }
            for (uint8_t p3 = PT_PAWN; p3 <= p2; ++p3) {
                ADD(k, p1, p2, p3, k);
                for (uint8_t p4 = PT_PAWN; p4 <= p3; ++p4) {
                    ADD(k, p1, p2, p3, p4, k);
                    for (uint8_t p5 = PT_PAWN; p5 <= p4; ++p5) {
                        ADD(k, p1, p2, p3, p4, p5, k);
                    }
                    for (uint8_t p5 = PT_PAWN; p5 < PT_KING; ++p5) {
                        ADD(k, p1, p2, p3, p4, k, p5);
                    }
                }
                for (uint8_t p4 = PT_PAWN; p4 < PT_KING; ++p4) {
                    ADD(k, p1, p2, p3, k, p4);
                    for (uint8_t p5 = PT_PAWN; p5 <= p4; ++p5) {
                        ADD(k, p1, p2, p3, k, p4, p5);
                    }
                }
            }
            for (uint8_t p3 = PT_PAWN; p3 <= p1; ++p3) {
                const uint8_t p4max = (p1 == p3) ? p2 : p3;
                for (uint8_t p4 = PT_PAWN; p4 <= p4max; ++p4) {
                    ADD(k, p1, p2, k, p3, p4);
                }
            }
        }
    }
}
