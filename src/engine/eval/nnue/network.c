#include "network.h"

#include "nnue_architecture.h"
#include "nnue_common.h"
#include "nnue_hash.h"
#include "nnue_parse.h"
#include "nnue_weight_storage.h"
#include "nnue_write.h"
#include "simd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <unistd.h>
    #define NNUE_HAVE_MMAP 1
#endif

static constexpr char InternalDir[] = "<internal>";

// Describe the .nnue header: the version, the architecture hash the file commits
// to, and the free-text description that follows it.
typedef struct {
    uint32_t hash_value;
    const uint8_t *description;
    size_t description_len;
} Header;

// Render a NUL-terminated message on the heap, or nullptr when the allocation fails.
static char *alloc_message(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int needed = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);
    if (needed < 0)
        return nullptr;

    char *out = malloc((size_t) needed + 1);
    if (out == nullptr)
        return nullptr;

    va_start(args, fmt);
    (void) vsnprintf(out, (size_t) needed + 1, fmt, args);
    va_end(args);
    return out;
}

void network_free_message(char *message) { free(message); }

// ---- byte-level reads --------------------------------------------------------

// Read a little-endian uint32_t at *OFFSET and advance it. Return false when the
// buffer is too short — the file is user input, so every read is bounded.
static bool read_u32_le(const uint8_t *bytes, size_t len, size_t *offset, uint32_t *out) {
    if (*offset + 4 > len)
        return false;
    *out = nnue_read_u32_le(bytes + *offset);
    *offset += 4;
    return true;
}

static bool read_header(const uint8_t *bytes, size_t len, size_t *offset, Header *out) {
    uint32_t version = 0;
    uint32_t hash_value = 0;
    uint32_t description_len_u32 = 0;
    if (!read_u32_le(bytes, len, offset, &version))
        return false;
    if (!read_u32_le(bytes, len, offset, &hash_value))
        return false;
    if (!read_u32_le(bytes, len, offset, &description_len_u32))
        return false;
    if (version != NNUE_VERSION)
        return false;

    const size_t description_len = (size_t) description_len_u32;
    if (*offset + description_len > len)
        return false;

    out->hash_value = hash_value;
    out->description = bytes + *offset;
    out->description_len = description_len;
    *offset += description_len;
    return true;
}

// ---- section parses ----------------------------------------------------------

#if MCFISH_SIMD_VECTOR && (defined(__AVX512BW__) || (defined(__AVX2__) && !defined(__AVX512F__)))
// State the pack order ONCE, for the load's permutation and the export's inverse.
//
// The two are a forward and a backward reading of the same table, in one file
// deliberately: an export that undid a permutation the load no longer applies would
// write a net nothing can read, and the only instrument that could see it is
// `net-roundtrip`. Sharing the table means there is nothing to keep in step.
    #if defined(__AVX512BW__)
static constexpr size_t PackusOrder[8] = { 0, 2, 4, 6, 1, 3, 5, 7 };
    #else
static constexpr size_t PackusOrder[8] = { 0, 2, 1, 3, 4, 6, 5, 7 };
    #endif

// Reorder every 64-lane span of an accumulator-side array so the transform's
// vpackuswb lands the u8 outputs in canonical order with no cross-lane permute
// (upstream permute_weights / PackusEpi16Order). Apply it to every array the 16-bit
// accumulator is built from — biases, psq weight rows, threat weight rows — and to
// nothing else; all other consumers add these arrays lane-wise, where one fixed
// permutation applied to every writer is invisible, and the transform's packus
// undoes it exactly.
//
// THE TABLE IS THE PACK WIDTH'S, so it moves with the tier. vpackuswb interleaves its
// two operands per 128-bit lane, so over one step's span the output block order is
// (0,2,1,3) per 32 lanes at 256-bit and (0,4,1,5,2,6,3,7) per 64 lanes at 512-bit;
// what the loader must apply is the inverse of that, which is why the 256-bit entry
// is self-inverse and the 512-bit one is not. Keep this paired with the step body in
// nnue_accumulator.c: nothing but `signature` and `simd-scalar` holds them together.
static void permute_packus_order(void *data, size_t elem_bytes, size_t count) {
    const size_t *const order = PackusOrder;
    const size_t block = 8 * elem_bytes;
    const size_t chunk = 8 * block;
    unsigned char *cursor = data;
    unsigned char buffer[8 * 8 * sizeof(int16_t)];
    for (size_t i = 0; i < count * elem_bytes; i += chunk) {
        for (size_t j = 0; j < 8; j++)
            memcpy(buffer + j * block, cursor + i + order[j] * block, block);
        memcpy(cursor + i, buffer, chunk);
    }
}

// Undo permute_packus_order, so `export_net` writes the order the FILE holds rather
// than the order this build's transform wants. Reading the same table backwards --
// `out[order[j]] = in[j]` against `out[j] = in[order[j]]` -- is what makes the two
// inverse whatever the table says; only the 256-bit table happens to be self-inverse.
static void unpermute_packus_order(void *data, size_t elem_bytes, size_t count) {
    const size_t *const order = PackusOrder;
    const size_t block = 8 * elem_bytes;
    const size_t chunk = 8 * block;
    unsigned char *cursor = data;
    unsigned char buffer[8 * 8 * sizeof(int16_t)];
    for (size_t i = 0; i < count * elem_bytes; i += chunk) {
        for (size_t j = 0; j < 8; j++)
            memcpy(buffer + order[j] * block, cursor + i + j * block, block);
        memcpy(cursor + i, buffer, chunk);
    }
}
#endif

// Parse the feature transformer into the shared weight storage and advance
// *OFFSET. Report a malformed net or a failed allocation as a rejection rather
// than aborting: the file is user input and the storage is megabytes.
static bool read_feature_transformer(const uint8_t *bytes, size_t len, size_t *offset) {
    uint8_t *dst = nnue_ft_storage(NNUE_FT_TOTAL_BYTES);
    if (dst == nullptr)
        return false;

    const size_t remaining = len - *offset;
    size_t consumed = 0;
    if (!nnue_parse_feature_transformer(bytes + *offset, remaining, dst, &consumed))
        return false;
    if (consumed == 0 || consumed > remaining)
        return false;
#if MCFISH_SIMD_VECTOR && (defined(__AVX512BW__) || (defined(__AVX2__) && !defined(__AVX512F__)))
    permute_packus_order(dst + NNUE_FT_BIASES_OFF, sizeof(int16_t), NNUE_FT_BIASES_COUNT);
    permute_packus_order(dst + NNUE_FT_WEIGHTS_OFF, sizeof(int16_t), NNUE_FT_PSQ_WEIGHTS_COUNT);
    permute_packus_order(dst + NNUE_FT_THREAT_WEIGHTS_OFF, sizeof(int8_t),
                         NNUE_FT_THREAT_WEIGHTS_COUNT);
#endif
    *offset += consumed;
    return true;
}

// Parse one bucket's affine layers into the shared weight storage: skip the
// leading architecture component hash, then fc_0/fc_1/fc_2 biases and scrambled
// weights. Advance *OFFSET.
static bool read_layer(size_t bucket, const uint8_t *bytes, size_t len, size_t *offset) {
    const uint8_t *blob = bytes + *offset;
    const size_t blob_len = len - *offset;

    size_t pos = 4;  // architecture component hash
    // A blob too short to hold the hash cannot be sliced past it; reject rather
    // than trap.
    if (blob_len < pos)
        return false;

    for (size_t idx = 0; idx < NNUE_LAYERS_PER_STACK; ++idx) {
        const size_t wb = nnue_layer_weights_bytes(idx);
        const size_t bb = nnue_layer_biases_bytes(idx);
        uint8_t *bdst = nnue_layer_storage(bucket, idx, NNUE_LAYER_BIASES, bb);
        uint8_t *wdst = nnue_layer_storage(bucket, idx, NNUE_LAYER_WEIGHTS, wb);
        if (bdst == nullptr || wdst == nullptr)
            return false;

        size_t used = 0;
        if (!nnue_parse_layer(blob + pos, blob_len - pos, bdst, bb, wdst, wb, &used))
            return false;
        pos += used;
        if (pos > blob_len)
            return false;
    }

    *offset += pos;
    return true;
}

// ---- load --------------------------------------------------------------------

static bool load_network_bytes(const uint8_t *bytes,
                               size_t len,
                               const char *current_name,
                               size_t current_name_len) {
    size_t offset = 0;
    Header header;
    if (!read_header(bytes, len, &offset, &header))
        return false;
    if (header.hash_value != nnue_network_hash_value())
        return false;

    if (!read_feature_transformer(bytes, len, &offset))
        return false;

    for (size_t bucket = 0; bucket < NNUE_LAYER_STACKS; ++bucket) {
        if (!read_layer(bucket, bytes, len, &offset))
            return false;
    }

    if (offset != len)
        return false;

    nnue_set_loaded_state(current_name, current_name_len, (const char *) header.description,
                          header.description_len);
    // Trust the parse as the sole source of weights; the offset == len check above
    // verifies the consumed-byte count, and the eval gates verify the values.
    return true;
}

// Hold a net file made readable as one contiguous byte range, plus how to release
// it. A memory map costs no 90 MB heap buffer and no read() copy: the parse faults
// the file's page-cache pages in as it walks them, sharing them read-only. `mapped`
// distinguishes the two release paths; the heap fallback covers a filesystem that
// cannot be mapped.
typedef struct {
    uint8_t *bytes;
    size_t len;
    bool mapped;
} NetFile;

// Read PATH whole into a heap buffer. Return nullptr when the file is missing, empty,
// or unreadable. The parse-from-a-map path falls back to this.
static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr)
        return nullptr;

    uint8_t *bytes = nullptr;
    if (fseek(file, 0, SEEK_END) != 0)
        goto done;

    {
        const long size = ftell(file);
        if (size <= 0)
            goto done;
        if (fseek(file, 0, SEEK_SET) != 0)
            goto done;

        const size_t len = (size_t) size;
        bytes = malloc(len);
        if (bytes == nullptr)
            goto done;
        if (fread(bytes, 1, len, file) != len) {
            free(bytes);
            bytes = nullptr;
            goto done;
        }
        *out_len = len;
    }

done:
    fclose(file);
    return bytes;
}

// Map PATH read-only, or read it whole where a map is unavailable. Leave `bytes`
// nullptr on any failure. The map is MAP_PRIVATE, so the parse reads a stable snapshot
// even if the file changes underneath it.
static NetFile open_net_file(const char *path) {
    NetFile f = { nullptr, 0, false };

#if defined(NNUE_HAVE_MMAP)
    const int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            const size_t len = (size_t) st.st_size;
            void *m = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
            if (m != MAP_FAILED) {
                f.bytes = m;
                f.len = len;
                f.mapped = true;
            }
        }
        close(fd);  // the mapping keeps its own reference; the descriptor is done
    }
    if (f.bytes != nullptr)
        return f;
#endif

    f.bytes = read_file(path, &f.len);
    f.mapped = false;
    return f;
}

static void close_net_file(NetFile *f) {
    if (f->bytes == nullptr)
        return;
#if defined(NNUE_HAVE_MMAP)
    if (f->mapped) {
        munmap(f->bytes, f->len);
        f->bytes = nullptr;
        return;
    }
#endif
    free(f->bytes);
    f->bytes = nullptr;
}

static void load_user_net(const char *dir, size_t dir_len, const char *name, size_t name_len) {
    nnue_mark_initialized();

    // Concatenate with no separator, as upstream does: the root directory already
    // carries its trailing separator.
    char *path = malloc(dir_len + name_len + 1);
    if (path == nullptr)
        return;
    if (dir_len != 0)
        memcpy(path, dir, dir_len);
    if (name_len != 0)
        memcpy(path + dir_len, name, name_len);
    path[dir_len + name_len] = '\0';

    NetFile f = open_net_file(path);
    free(path);
    if (f.bytes == nullptr)
        return;

    (void) load_network_bytes(f.bytes, f.len, name, name_len);
    close_net_file(&f);
}

// Load the internal net. mcfish embeds none — the net is a runtime input — so the
// blob is a one-byte stub, which fails the header read and leaves the search to
// fall through to the on-disk candidates. The branch is kept so the candidate
// order matches upstream's.
static void load_internal(void) {
    nnue_mark_initialized();

    static constexpr uint8_t EmbeddedStub[1] = { 0 };
    (void) load_network_bytes(EmbeddedStub, sizeof EmbeddedStub, NETWORK_DEFAULT_EVAL_FILE_NAME,
                              sizeof(NETWORK_DEFAULT_EVAL_FILE_NAME) - 1);
}

typedef struct {
    const char *ptr;
    size_t len;
} Slice;

static bool slice_equals(Slice a, const char *b, size_t b_len) {
    return a.len == b_len && (a.len == 0 || memcmp(a.ptr, b, a.len) == 0);
}

void network_load(const char *root_directory,
                  size_t root_directory_len,
                  const char *evalfile_path,
                  size_t evalfile_path_len) {
    const char *name = evalfile_path_len == 0 ? NETWORK_DEFAULT_EVAL_FILE_NAME : evalfile_path;
    const size_t name_len =
      evalfile_path_len == 0 ? sizeof(NETWORK_DEFAULT_EVAL_FILE_NAME) - 1 : evalfile_path_len;

    const Slice dirs[3] = {
        { InternalDir, sizeof InternalDir - 1 },
        { "", 0 },
        { root_directory == nullptr ? "" : root_directory,
          root_directory == nullptr ? 0 : root_directory_len },
    };

    for (size_t i = 0; i < 3; ++i) {
        // Stop as soon as the wanted net is the resident one; a later candidate
        // must not overwrite an earlier success.
        if (nnue_equal_current_name(name, name_len))
            continue;

        const bool is_internal = slice_equals(dirs[i], InternalDir, sizeof InternalDir - 1);
        if (!is_internal)
            load_user_net(dirs[i].ptr, dirs[i].len, name, name_len);

        if (is_internal
            && slice_equals((Slice) { name, name_len }, NETWORK_DEFAULT_EVAL_FILE_NAME,
                            sizeof(NETWORK_DEFAULT_EVAL_FILE_NAME) - 1))
            load_internal();
    }
}

// ---- verify ------------------------------------------------------------------

NetworkVerifyResult network_verify(const char *evalfile_path, size_t evalfile_path_len) {
    const char *name = evalfile_path_len == 0 ? NETWORK_DEFAULT_EVAL_FILE_NAME : evalfile_path;
    const size_t name_len =
      evalfile_path_len == 0 ? sizeof(NETWORK_DEFAULT_EVAL_FILE_NAME) - 1 : evalfile_path_len;

    if (!nnue_equal_current_name(name, name_len)) {
        return (NetworkVerifyResult) {
            .should_exit = true,
            .message = alloc_message(
              "ERROR: Network evaluation parameters compatible with the engine must be "
              "available.\n"
              "ERROR: The network file %.*s was not loaded successfully.\n"
              "ERROR: The UCI option EvalFile might need to specify the full path, including "
              "the directory name, to the network file.\n"
              "ERROR: The default net can be downloaded from: "
              "https://tests.stockfishchess.org/api/nn/%s\n"
              "ERROR: The engine will be terminated now.\n",
              (int) name_len, name, NETWORK_DEFAULT_EVAL_FILE_NAME),
        };
    }

    // Fix the verification dimensions by the NNUE architecture: sizeof the
    // FeatureTransformer plus NetworkArchitecture * LayerStacks, and the static
    // InputDimensions / TransformedFeatureDimensions / FC_0_OUTPUTS /
    // FC_1_OUTPUTS (upstream nnue/network.cpp:192).
    //
    // DERIVE both from the architecture rather than pinning the two totals: the
    // banner is the one place the whole net shape is visible to a user, and a pinned
    // total goes stale silently on an architecture change — nothing recomputes it and
    // no gate reads it. Only the per-stack size stays a constant, because it is
    // sizeof(NetworkArchitecture), a padded C++ object mcfish does not mirror field
    // for field.
    const size_t network_architecture_bytes = 35328;
    const size_t size_bytes = NNUE_FT_TOTAL_BYTES + network_architecture_bytes * NNUE_LAYER_STACKS;
    const size_t input_dimensions = NNUE_PSQ_FEATURE_DIMENSIONS + NNUE_THREAT_AND_PAIR_DIMENSIONS;

    return (NetworkVerifyResult) {
        .should_exit = false,
        .message =
          alloc_message("NNUE evaluation using %.*s (%zuMiB, (%zu, %d, %d, %d, 1))", (int) name_len,
                        name, size_bytes / (1024 * 1024), input_dimensions,
                        NNUE_TRANSFORMED_FEATURE_DIMENSIONS, NNUE_FC_0_OUTPUTS, NNUE_FC_1_OUTPUTS),
    };
}

// ---- save --------------------------------------------------------------------
//
// The mirror of the parse above, and the ONE consumer of nnue_write.c. It exists
// because upstream's `export_net` does, and it is the only path in the tree that
// produces a .nnue rather than reading one -- so nothing else can hold it to the
// format. `./build.sh net-roundtrip` is what does: it exports the shipped net and
// compares it byte for byte with the file the loader read.

// Bound the scratch this walk needs. The regions run to 66 MB and upstream copies
// the whole transformer to unpermute it (a `make_unique<FeatureTransformer>(*this)`);
// span the walk instead, because the permutation is contained in 64-element chunks
// and every region count here is a multiple of one. 256 KiB is a whole number of
// chunks at every element width, and it is also >= the largest affine weight array,
// so one buffer serves both walks.
enum { SAVE_SPAN_BYTES = 1u << 18 };

// Copy N elements starting at FIRST out of a resident feature-transformer region,
// undoing the load-time permutation on the builds that applied one. FIRST and N must
// be multiples of the 64-element permutation chunk, which every region boundary and
// every span below is.
static void save_span(uint8_t *scratch,
                      const uint8_t *region,
                      size_t elem_bytes,
                      size_t first,
                      size_t n,
                      [[maybe_unused]] bool permuted) {
    memcpy(scratch, region + first * elem_bytes, n * elem_bytes);
#if MCFISH_SIMD_VECTOR && (defined(__AVX512BW__) || (defined(__AVX2__) && !defined(__AVX512F__)))
    if (permuted)
        unpermute_packus_order(scratch, elem_bytes, n);
#endif
}

// Emit a raw int8 region: the threat and pp weight blocks, which the file stores as
// plain bytes with no encoding and no byte order to apply.
static void save_raw_i8(NnueWriter *w,
                        const uint8_t *region,
                        size_t first,
                        size_t count,
                        bool permuted,
                        uint8_t *scratch) {
    for (size_t done = 0; done < count && w->ok;) {
        const size_t n = count - done < SAVE_SPAN_BYTES ? count - done : (size_t) SAVE_SPAN_BYTES;
        save_span(scratch, region, 1, first + done, n, permuted);
        nnue_write_bytes(w, scratch, n);
        done += n;
    }
}

// Emit one LEB128 section over a region, in two passes: a section states its byte
// count before its bytes, and the largest of these is 23 million values.
static void
save_leb_i16(NnueWriter *w, const uint8_t *region, size_t count, bool permuted, uint8_t *scratch) {
    const size_t span = SAVE_SPAN_BYTES / sizeof(int16_t);
    size_t bytes = 0;
    for (size_t done = 0; done < count; done += span) {
        const size_t n = count - done < span ? count - done : span;
        save_span(scratch, region, sizeof(int16_t), done, n, permuted);
        bytes += nnue_leb_bytes_i16((const int16_t *) (const void *) scratch, n);
    }

    nnue_write_leb_header(w, (uint32_t) bytes);
    for (size_t done = 0; done < count && w->ok; done += span) {
        const size_t n = count - done < span ? count - done : span;
        save_span(scratch, region, sizeof(int16_t), done, n, permuted);
        nnue_write_leb_i16(w, (const int16_t *) (const void *) scratch, n);
    }
}

static void
save_leb_i32(NnueWriter *w, const uint8_t *region, size_t first, size_t count, uint8_t *scratch) {
    const size_t span = SAVE_SPAN_BYTES / sizeof(int32_t);
    size_t bytes = 0;
    for (size_t done = 0; done < count; done += span) {
        const size_t n = count - done < span ? count - done : span;
        save_span(scratch, region, sizeof(int32_t), first + done, n, false);
        bytes += nnue_leb_bytes_i32((const int32_t *) (const void *) scratch, n);
    }

    nnue_write_leb_header(w, (uint32_t) bytes);
    for (size_t done = 0; done < count && w->ok; done += span) {
        const size_t n = count - done < span ? count - done : span;
        save_span(scratch, region, sizeof(int32_t), first + done, n, false);
        nnue_write_leb_i32(w, (const int32_t *) (const void *) scratch, n);
    }
}

// Write the feature transformer in the file's own order, which is
// nnue_parse_feature_transformer's read order run backwards: the component hash, then
// biases, the threat weight/psqt pair, the pp weight/psqt pair, and finally the psq
// weights and their psqt block. The two concatenated regions are split at the same
// boundary the parse joined them on.
static void save_feature_transformer(NnueWriter *w, const uint8_t *ft, uint8_t *scratch) {
    nnue_write_u32_le(w, nnue_feature_transformer_hash_value());

    save_leb_i16(w, ft + NNUE_FT_BIASES_OFF, NNUE_FT_BIASES_COUNT, true, scratch);

    save_raw_i8(w, ft + NNUE_FT_THREAT_WEIGHTS_OFF, 0, NNUE_FT_THREAT_ONLY_WEIGHTS_COUNT, true,
                scratch);
    save_leb_i32(w, ft + NNUE_FT_THREAT_PSQT_WEIGHTS_OFF, 0, NNUE_FT_THREAT_ONLY_PSQT_COUNT,
                 scratch);
    save_raw_i8(w, ft + NNUE_FT_THREAT_WEIGHTS_OFF, NNUE_FT_THREAT_ONLY_WEIGHTS_COUNT,
                NNUE_FT_PAIR_ONLY_WEIGHTS_COUNT, true, scratch);
    save_leb_i32(w, ft + NNUE_FT_THREAT_PSQT_WEIGHTS_OFF, NNUE_FT_THREAT_ONLY_PSQT_COUNT,
                 NNUE_FT_PAIR_ONLY_PSQT_COUNT, scratch);

    save_leb_i16(w, ft + NNUE_FT_WEIGHTS_OFF, NNUE_FT_PSQ_WEIGHTS_COUNT, true, scratch);
    save_leb_i32(w, ft + NNUE_FT_PSQT_WEIGHTS_OFF, 0, NNUE_FT_PSQT_WEIGHTS_COUNT, scratch);
}

// Write one bucket's three affine layers: the stack's component hash, then each
// layer's biases little-endian and its weights back in FILE order. The weights are
// held under the SSSE3 scramble, so the walk indexes through it exactly as upstream's
// `weights[get_weight_index(i)]` does -- the same expression the parse writes through,
// read the other way.
static void save_layer_stack(NnueWriter *w, size_t bucket, uint8_t *scratch) {
    nnue_write_u32_le(w, nnue_architecture_hash_value());

    for (size_t idx = 0; idx < NNUE_LAYERS_PER_STACK && w->ok; ++idx) {
        const NnueLayerDims dims = nnue_layer_dims(idx);
        const size_t bb = nnue_layer_biases_bytes(idx);
        const size_t wb = nnue_layer_weights_bytes(idx);
        const uint8_t *const biases = nnue_layer_ptr(bucket, idx, NNUE_LAYER_BIASES);
        const uint8_t *const weights = nnue_layer_ptr(bucket, idx, NNUE_LAYER_WEIGHTS);
        if (biases == nullptr || weights == nullptr) {
            w->ok = false;
            return;
        }

        nnue_write_i32_le(w, (const int32_t *) (const void *) biases, bb / sizeof(int32_t));

        for (size_t i = 0; i < wb; ++i)
            scratch[i] = weights[nnue_weight_index_scrambled(i, dims.padded_input_dimensions,
                                                             dims.output_dimensions)];
        nnue_write_bytes(w, scratch, wb);
    }
}

NetworkSaveResult network_save(const char *filename) {
    size_t current_len = 0;
    (void) nnue_nn_current(&current_len);
    if (current_len == 0 || nnue_ft_ptr() == nullptr) {
        return (NetworkSaveResult) {
            .saved = false,
            .message = alloc_message("Failed to export a net. No network file is currently "
                                     "loaded. Please load a network file first."),
        };
    }

    // Upstream's rule, and the reason it has one: with no filename the net is written
    // to the DEFAULT name, so a net loaded under another name would silently overwrite
    // the default file with different weights (network.cpp:122).
    const bool current_is_default = nnue_equal_current_name(
      NETWORK_DEFAULT_EVAL_FILE_NAME, sizeof(NETWORK_DEFAULT_EVAL_FILE_NAME) - 1);
    if (filename == nullptr && !current_is_default) {
        return (NetworkSaveResult) {
            .saved = false,
            .message = alloc_message("Failed to export a net. A non-embedded net can only be "
                                     "saved if the filename is specified"),
        };
    }

    const char *const actual = filename != nullptr ? filename : NETWORK_DEFAULT_EVAL_FILE_NAME;

    uint8_t *const scratch = malloc(SAVE_SPAN_BYTES);
    FILE *const out = scratch != nullptr ? fopen(actual, "wb") : nullptr;
    bool saved = false;
    if (out != nullptr) {
        NnueWriter w = { .out = out, .ok = true };

        size_t description_len = 0;
        const char *const description = nnue_nn_description(&description_len);

        nnue_write_u32_le(&w, NNUE_VERSION);
        nnue_write_u32_le(&w, nnue_network_hash_value());
        nnue_write_u32_le(&w, (uint32_t) description_len);
        nnue_write_bytes(&w, description, description_len);

        save_feature_transformer(&w, nnue_ft_ptr(), scratch);
        for (size_t bucket = 0; bucket < NNUE_LAYER_STACKS && w.ok; ++bucket)
            save_layer_stack(&w, bucket, scratch);

        // Close before reporting: the last megabytes are still in the stdio buffer
        // until it is flushed, so a full disk is a failure this would otherwise miss.
        saved = w.ok && fclose(out) == 0;
    }
    free(scratch);

    return (NetworkSaveResult) {
        .saved = saved,
        .message = saved ? alloc_message("Network saved successfully to %s", actual)
                         : alloc_message("Failed to export a net"),
    };
}

// ---- forward pass ------------------------------------------------------------

NnueEvalOutput network_evaluate(const Position *pos, void *accumulator_stack, void *refresh_cache) {
    return nnue_inference_evaluate(pos, accumulator_stack, refresh_cache);
}

NnueTraceOutput
network_trace_evaluate(const Position *pos, void *accumulator_stack, void *refresh_cache) {
    return nnue_inference_trace_evaluate(pos, accumulator_stack, refresh_cache);
}
