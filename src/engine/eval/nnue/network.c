#include "network.h"

#include "nnue_architecture.h"
#include "nnue_common.h"
#include "nnue_hash.h"
#include "nnue_parse.h"
#include "nnue_weight_storage.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char InternalDir[] = "<internal>";

// Describe the .nnue header: the version, the architecture hash the file commits
// to, and the free-text description that follows it.
typedef struct {
    uint32_t hash_value;
    const uint8_t *description;
    size_t description_len;
} Header;

// Render a NUL-terminated message on the heap, or NULL when the allocation fails.
static char *alloc_message(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0)
        return NULL;

    char *out = malloc((size_t) needed + 1);
    if (out == NULL)
        return NULL;

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

// Parse the feature transformer into the shared weight storage and advance
// *OFFSET. Report a malformed net or a failed allocation as a rejection rather
// than aborting: the file is user input and the storage is megabytes.
static bool read_feature_transformer(const uint8_t *bytes, size_t len, size_t *offset) {
    uint8_t *dst = nnue_ft_storage(NNUE_FT_TOTAL_BYTES);
    if (dst == NULL)
        return false;

    const size_t remaining = len - *offset;
    size_t consumed = 0;
    if (!nnue_parse_feature_transformer(bytes + *offset, remaining, dst, &consumed))
        return false;
    if (consumed == 0 || consumed > remaining)
        return false;
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
        if (bdst == NULL || wdst == NULL)
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

// Read PATH whole into a heap buffer, storing its length in *OUT_LEN. Return NULL
// when the file is missing, empty, or unreadable.
static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;

    uint8_t *bytes = NULL;
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
        if (bytes == NULL)
            goto done;
        if (fread(bytes, 1, len, file) != len) {
            free(bytes);
            bytes = NULL;
            goto done;
        }
        *out_len = len;
    }

done:
    fclose(file);
    return bytes;
}

static void load_user_net(const char *dir, size_t dir_len, const char *name, size_t name_len) {
    nnue_mark_initialized();

    // Concatenate with no separator, as upstream does: the root directory already
    // carries its trailing separator.
    char *path = malloc(dir_len + name_len + 1);
    if (path == NULL)
        return;
    if (dir_len != 0)
        memcpy(path, dir, dir_len);
    if (name_len != 0)
        memcpy(path + dir_len, name, name_len);
    path[dir_len + name_len] = '\0';

    size_t len = 0;
    uint8_t *bytes = read_file(path, &len);
    free(path);
    if (bytes == NULL)
        return;

    (void) load_network_bytes(bytes, len, name, name_len);
    free(bytes);
}

// Load the internal net. mcfish embeds none — the net is a runtime input — so the
// blob is the same one-byte stub zfish carries, which fails the header read and
// leaves the search to fall through to the on-disk candidates. The branch is kept
// so the candidate order matches upstream's.
static void load_internal(void) {
    nnue_mark_initialized();

    static const uint8_t EmbeddedStub[1] = { 0 };
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
        { root_directory == NULL ? "" : root_directory,
          root_directory == NULL ? 0 : root_directory_len },
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
    // FC_1_OUTPUTS. Fixed constants.
    const size_t size_bytes = 111263232;
    const size_t input_dimensions = 83248;

    return (NetworkVerifyResult) {
        .should_exit = false,
        .message =
          alloc_message("NNUE evaluation using %.*s (%zuMiB, (%zu, %d, %d, %d, 1))", (int) name_len,
                        name, size_bytes / (1024 * 1024), input_dimensions,
                        NNUE_TRANSFORMED_FEATURE_DIMENSIONS, NNUE_FC_0_OUTPUTS, NNUE_FC_1_OUTPUTS),
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
