#include "redstone_c.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* A canonical, padding-independent description of everything the generated
 * queries depend on. Dynamic values are intentionally omitted, so a model
 * compiled once can be attached to many compatible initial states. */
static StateId topology_state(BlockState block)
{
    BlockState key = block_default((BlockId)block.id);
    switch ((BlockId)block.id) {
    case BLOCK_REDSTONE_WIRE:
        key.data.wire = block.data.wire;
        key.data.wire.power = 0;
        break;
    case BLOCK_REDSTONE_TORCH:
        key.data.torch.support_dir = block.data.torch.support_dir;
        break;
    case BLOCK_REDSTONE_REPEATER:
        key.data.repeater.facing = block.data.repeater.facing;
        break;
    case BLOCK_REDSTONE_COMPARATOR:
        key.data.comparator.facing = block.data.comparator.facing;
        break;
    case BLOCK_OBSERVER:
        key.data.observer.facing = block.data.observer.facing;
        break;
    case BLOCK_LEVER:
        key.data.lever.support_dir = block.data.lever.support_dir;
        key.data.lever.facing = block.data.lever.facing;
        break;
    case BLOCK_REDSTONE_LAMP:
    case BLOCK_NOTE_BLOCK:
    case BLOCK_BARREL:
    case BLOCK_HOPPER:
    case BLOCK_DROPPER:
        break;
    case BLOCK_POWERED_RAIL:
    case BLOCK_ACTIVATOR_RAIL:
        key.data.rail.facing = block.data.rail.facing;
        key.data.rail.ascending = block.data.rail.ascending;
        break;
    case BLOCK_PISTON:
    case BLOCK_STICKY_PISTON:
        key.data.piston.facing = block.data.piston.facing;
        key.data.piston.extended = block.data.piston.extended;
        break;
    default:
        key = block;
        break;
    }
    StateId encoded = 0;
    (void)state_encode(key, &encoded); /* Validated by redstone_state_build. */
    return encoded;
}

typedef struct {
    FILE *output;
    const uint32_t *expected;
    size_t count, limit;
    bool matches;
} Topology;

static void word(Topology *sink, uint32_t value)
{
    if (sink->output) {
        if (sink->count % 8 == 0) fputs("\n    ", sink->output);
        fprintf(sink->output, "UINT32_C(%" PRIu32 "),", value);
    } else if (!sink->expected || sink->count >= sink->limit ||
               sink->expected[sink->count] != value) {
        sink->matches = false;
    }
    ++sink->count;
}

static void topology(const RedstonePlan *plan, Topology *sink)
{
    word(sink, SIM_C_ABI_VERSION);
    /* The model describes a shape, not its location in a world. Differences
     * use unsigned arithmetic so negative coordinates and large spans work. */
    word(sink, (uint32_t)plan->bounds.max.x - (uint32_t)plan->bounds.min.x);
    word(sink, (uint32_t)plan->bounds.max.y - (uint32_t)plan->bounds.min.y);
    word(sink, (uint32_t)plan->bounds.max.z - (uint32_t)plan->bounds.min.z);
    word(sink, (uint32_t)plan->node_count);
    word(sink, (uint32_t)plan->input_count);
    for (size_t i = 0; i < plan->node_count; ++i) {
        RedstonePlanNode node = plan->nodes[i];
        word(sink, (uint32_t)node.pos.x - (uint32_t)plan->bounds.min.x);
        word(sink, (uint32_t)node.pos.y - (uint32_t)plan->bounds.min.y);
        word(sink, (uint32_t)node.pos.z - (uint32_t)plan->bounds.min.z);
        word(sink, topology_state(node.initial_state));
        word(sink, node.flags);
        word(sink, node.wire_output_faces);
        word(sink, node.first_input);
        word(sink, node.input_count);
    }
    for (size_t i = 0; i < plan->input_count; ++i) {
        RedstonePlanInput input = plan->inputs[i];
        word(sink, input.source);
        word(sink, input.kind);
        word(sink, input.source_face);
        word(sink, input.attenuation);
    }
}

bool redstone_c_matches(const RedstonePlan *plan, const SimCModel *model)
{
    if (!plan || plan->live_inputs || !model || model->abi_version != SIM_C_ABI_VERSION ||
        model->node_count != plan->node_count || !model->evaluate ||
        !model->topology) return false;
    Topology sink = {.expected = model->topology,
                     .limit = model->topology_word_count, .matches = true};
    topology(plan, &sink);
    if (!sink.matches || sink.count != sink.limit) return false;
    if (!model->read_offsets)
        return !model->read_count && !model->read_targets;
    if (model->read_offsets[0] != 0 ||
        model->read_offsets[model->node_count] != model->read_count ||
        (model->read_count && !model->read_targets)) return false;
    for (size_t i = 0; i < model->node_count; ++i)
        if (model->read_offsets[i] > model->read_offsets[i + 1]) return false;
    for (size_t i = 0; i < model->read_count; ++i)
        if (model->read_targets[i] >= model->node_count) return false;
    return true;
}

static bool symbol_valid(const char *symbol)
{
    if (!symbol || !((*symbol >= 'a' && *symbol <= 'z') ||
                     (*symbol >= 'A' && *symbol <= 'Z'))) return false;
    size_t length = 0;
    for (const char *p = symbol; *p; ++p) {
        if (++length > 128 || !((*p >= 'a' && *p <= 'z') ||
            (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '_')) return false;
    }
    static const char *const keywords[] = {
        "auto", "break", "case", "char", "const", "continue", "default",
        "do", "double", "else", "enum", "extern", "float", "for", "goto",
        "if", "inline", "int", "long", "register", "restrict", "return",
        "short", "signed", "sizeof", "static", "struct", "switch",
        "typedef", "union", "unsigned", "void", "volatile", "while",
        /* Macros/types used by the generated unit are not valid model names. */
        "bool", "true", "false", "NULL", "size_t", "uint8_t", "uint32_t",
        "BlockState", "SimCModel", "SimCEvaluation", "RsResult",
        "SIM_C_ABI_VERSION", "UINT32_C"
    };
    for (size_t i = 0; i < sizeof(keywords)/sizeof(*keywords); ++i)
        if (strcmp(symbol, keywords[i]) == 0) return false;
    return true;
}

/* Check the same supported cases as redstone_eval.c before emitting a node.
 * Conductors only recurse into DIRECT reads, so the walk has depth <= 2. */
static bool signal_supported(const RedstonePlan *plan, RedstoneNodeId id,
                              bool direct)
{
    RedstonePlanNode node = plan->nodes[id];
    if (node.flags & REDSTONE_NODE_BOUNDARY) return false;
    BlockState block = node.initial_state;
    switch ((BlockId)block.id) {
    case BLOCK_REDSTONE_BLOCK:
    case BLOCK_REDSTONE_WIRE:
    case BLOCK_REDSTONE_TORCH:
    case BLOCK_REDSTONE_REPEATER:
    case BLOCK_REDSTONE_COMPARATOR:
    case BLOCK_OBSERVER:
    case BLOCK_LEVER:
        break;
    default: {
        const BlockDef *definition = block_definition((BlockId)block.id);
        if (definition->signal || definition->direct_signal) return false;
        break;
    }
    }
    if (!direct && (node.flags & REDSTONE_NODE_CONDUCTOR)) {
        for (size_t i = 0; i < node.input_count; ++i) {
            RedstonePlanInput input = plan->inputs[node.first_input + i];
            if (input.kind != REDSTONE_INPUT_DIRECT) continue;
            if (input.source_face >= DIR_COUNT ||
                !signal_supported(plan, input.source, true)) return false;
        }
    }
    return true;
}

static bool node_supported(const RedstonePlan *plan, size_t id)
{
    RedstonePlanNode node = plan->nodes[id];
    if (node.flags & (REDSTONE_NODE_BOUNDARY | REDSTONE_NODE_RUNTIME |
                      REDSTONE_NODE_INPUT_FALLBACK)) return false;
    BlockId target = (BlockId)node.initial_state.id;
    switch (target) {
    case BLOCK_REDSTONE_WIRE:
    case BLOCK_REDSTONE_TORCH:
    case BLOCK_REDSTONE_REPEATER:
    case BLOCK_REDSTONE_COMPARATOR:
    case BLOCK_IRON:
    case BLOCK_SLIME:
    case BLOCK_TARGET:
        break;
    default:
        return false;
    }
    for (size_t i = 0; i < node.input_count; ++i) {
        RedstonePlanInput input = plan->inputs[node.first_input + i];
        RedstonePlanNode source = plan->nodes[input.source];
        if (source.flags & REDSTONE_NODE_BOUNDARY) return false;
        BlockId kind = (BlockId)source.initial_state.id;
        if (target == BLOCK_REDSTONE_COMPARATOR &&
            block_definition(kind)->analog_output) return false;
        if (kind == BLOCK_REDSTONE_WIRE &&
            (target == BLOCK_REDSTONE_REPEATER ||
             target == BLOCK_REDSTONE_COMPARATOR ||
             input.source_face == DIR_COUNT)) continue;
        if (input.kind == REDSTONE_INPUT_SIDE &&
            target == BLOCK_REDSTONE_COMPARATOR &&
            kind == BLOCK_REDSTONE_BLOCK) continue;
        if (input.source_face >= DIR_COUNT ||
            !signal_supported(plan, input.source,
                              input.kind != REDSTONE_INPUT_DEFAULT)) return false;
    }
    return true;
}

typedef struct {
    RedstoneNodeId source, target;
} CRead;

typedef struct {
    CRead *items;
    size_t count, capacity;
    RedstoneNodeId *seen;
} CReads;

typedef struct {
    char *data;
    size_t length, capacity;
    RsResult error;
    CReads *reads;
    RedstoneNodeId target;
} CText;

/* Record at the same sites that emit dynamic state loads. In particular,
 * constant faces and conductor relays must not grow a different read graph
 * from the generated query. Each source/target pair is retained only once. */
static void state_read(CText *out, RedstoneNodeId source)
{
    CReads *reads = out->reads;
    if (out->error != RS_OK || !reads || reads->seen[source] == out->target) return;
    if (reads->count == reads->capacity) {
        size_t capacity = reads->capacity ? reads->capacity * 2 : 64;
        if (capacity < reads->capacity || capacity > SIZE_MAX / sizeof(*reads->items)) {
            out->error = RS_OUT_OF_MEMORY;
            return;
        }
        CRead *items = realloc(reads->items, capacity * sizeof(*items));
        if (!items) { out->error = RS_OUT_OF_MEMORY; return; }
        reads->items = items;
        reads->capacity = capacity;
    }
    reads->seen[source] = out->target;
    reads->items[reads->count++] = (CRead){source, out->target};
}

static void text_printf(CText *out, const char *format, ...)
{
    if (out->error != RS_OK) return;
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    int count = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (count < 0) out->error = RS_INTERNAL_ERROR;
    else if (out->length > SIZE_MAX - (size_t)count - 1)
        out->error = RS_OUT_OF_MEMORY;
    else {
        size_t needed = out->length + (size_t)count + 1;
        if (needed > out->capacity) {
            size_t capacity = out->capacity ? out->capacity : 256;
            while (capacity < needed) {
                if (capacity > SIZE_MAX/2) { capacity = needed; break; }
                capacity *= 2;
            }
            char *data = realloc(out->data, capacity);
            if (!data) out->error = RS_OUT_OF_MEMORY;
            else { out->data = data; out->capacity = capacity; }
        }
        if (out->error == RS_OK) {
            vsnprintf(out->data + out->length, out->capacity - out->length, format, args);
            out->length += (size_t)count;
        }
    }
    va_end(args);
}

static void text_puts(const char *text, CText *out)
{
    text_printf(out, "%s", text);
}

static unsigned emit_signal(CText *out, const RedstonePlan *plan,
                             RedstoneNodeId id, unsigned face,
                             bool without_wire, bool direct, unsigned *next,
                             size_t base)
{
    RedstonePlanNode node = plan->nodes[id];
    BlockState block = node.initial_state;
    int64_t offset = (int64_t)id - (int64_t)base;
    unsigned value = (*next)++;
    text_printf(out, "    uint8_t v%u = ", value);
    switch ((BlockId)block.id) {
    case BLOCK_REDSTONE_BLOCK:
        text_printf(out, "%u", direct ? 0u : 15u);
        break;
    case BLOCK_REDSTONE_WIRE:
        if (without_wire || face == DIR_UP ||
            (face != DIR_DOWN && !(node.wire_output_faces & (1u << face))))
            text_puts("0", out);
        else {
            state_read(out, id);
            text_printf(out, "blocks[%" PRId64 "].data.wire.power", offset);
        }
        break;
    case BLOCK_REDSTONE_TORCH:
        if (face == block.data.torch.support_dir || (direct && face != DIR_UP))
            text_puts("0", out);
        else {
            state_read(out, id);
            text_printf(out, "blocks[%" PRId64 "].data.torch.lit ? 15 : 0", offset);
        }
        break;
    case BLOCK_REDSTONE_REPEATER:
        if (face != block.data.repeater.facing) text_puts("0", out);
        else {
            state_read(out, id);
            text_printf(out, "blocks[%" PRId64 "].data.repeater.powered ? 15 : 0", offset);
        }
        break;
    case BLOCK_REDSTONE_COMPARATOR:
        if (face != block.data.comparator.facing) text_puts("0", out);
        else {
            state_read(out, id);
            text_printf(out, "blocks[%" PRId64 "].data.comparator.powered ? "
                        "blocks[%" PRId64 "].data.comparator.output : 0", offset, offset);
        }
        break;
    case BLOCK_OBSERVER:
        if (face != block.data.observer.facing) text_puts("0", out);
        else {
            state_read(out, id);
            text_printf(out, "blocks[%" PRId64 "].data.observer.powered ? 15 : 0", offset);
        }
        break;
    case BLOCK_LEVER:
        if (direct && face != block.data.lever.support_dir) text_puts("0", out);
        else {
            state_read(out, id);
            text_printf(out, "blocks[%" PRId64 "].data.lever.powered ? 15 : 0", offset);
        }
        break;
    default:
        text_puts("0", out);
        break;
    }
    text_puts(";\n", out);
    if (!direct && (node.flags & REDSTONE_NODE_CONDUCTOR)) {
        for (size_t i = 0; i < node.input_count; ++i) {
            RedstonePlanInput input = plan->inputs[node.first_input + i];
            if (input.kind != REDSTONE_INPUT_DIRECT) continue;
            unsigned child = emit_signal(out, plan, input.source,
                input.source_face, without_wire, true, next, base);
            text_printf(out, "    if (v%u > v%u) v%u = v%u;\n",
                    child, value, value, child);
        }
    }
    return value;
}

static void emit_body(CText *out, const RedstonePlan *plan, size_t id)
{
    RedstonePlanNode node = plan->nodes[id];
    BlockId target = (BlockId)node.initial_state.id;
    text_puts("    (void)blocks;\n    SimCEvaluation r = {0};\n", out);
    unsigned next = 0;
    bool has_side = false;
    for (size_t i = 0; i < node.input_count; ++i) {
        RedstonePlanInput input = plan->inputs[node.first_input + i];
        BlockId source = (BlockId)plan->nodes[input.source].initial_state.id;
        unsigned value;
        if (input.kind == REDSTONE_INPUT_SIDE) has_side = true;
        if (source == BLOCK_REDSTONE_WIRE &&
            (target == BLOCK_REDSTONE_REPEATER ||
             target == BLOCK_REDSTONE_COMPARATOR || input.source_face == DIR_COUNT)) {
            value = next++;
            state_read(out, input.source);
            text_printf(out, "    uint8_t v%u = blocks[%" PRId64 "].data.wire.power;\n",
                    value, (int64_t)input.source - (int64_t)id);
        } else if (input.kind == REDSTONE_INPUT_SIDE &&
                   target == BLOCK_REDSTONE_COMPARATOR && source == BLOCK_REDSTONE_BLOCK) {
            value = next++;
            text_printf(out, "    uint8_t v%u = 15;\n", value);
        } else {
            value = emit_signal(out, plan, input.source, input.source_face,
                target == BLOCK_REDSTONE_WIRE,
                input.kind != REDSTONE_INPUT_DEFAULT, &next, id);
        }
        if (input.attenuation)
            text_printf(out, "    v%u = v%u > %u ? (uint8_t)(v%u - %u) : 0;\n",
                    value, value, input.attenuation, value, input.attenuation);
        const char *channel = input.kind == REDSTONE_INPUT_SIDE ? "side" : "input";
        text_printf(out, "    if (v%u > r.%s) r.%s = v%u;\n",
                value, channel, channel, value);
    }
    text_puts("    r.target_on = r.input > 0;\n    r.target_power = r.input;\n", out);
    if (target == BLOCK_REDSTONE_TORCH) {
        text_puts("    r.target_on = !r.target_on;\n"
              "    r.target_power = r.target_on ? 15 : 0;\n", out);
    } else if (target == BLOCK_REDSTONE_REPEATER) {
        if (has_side) {
            state_read(out, (RedstoneNodeId)id);
            text_puts("    r.locked = r.side > 0;\n"
                "    if (r.locked) r.target_on = blocks[0].data.repeater.powered;\n", out);
        }
        text_puts("    r.target_power = r.target_on ? 15 : 0;\n", out);
    } else if (target == BLOCK_REDSTONE_COMPARATOR) {
        state_read(out, (RedstoneNodeId)id);
        text_puts("    r.target_on = r.input > 0 && (r.input > r.side ||\n"
            "        (r.input == r.side && !blocks[0].data.comparator.subtract));\n"
            "    if (blocks[0].data.comparator.subtract)\n"
            "        r.target_power = r.input > r.side ? (uint8_t)(r.input - r.side) : 0;\n", out);
    }
    text_puts("    *output = r;\n    return RS_OK;\n", out);
}

typedef struct {
    CText body;
    uint64_t hash;
    uint32_t next;
    size_t first_node;
} Query;

static uint64_t text_hash(const CText *text)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < text->length; ++i) {
        hash ^= (unsigned char)text->data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

RsResult redstone_c_write(const RedstonePlan *plan, FILE *out,
                          const char *symbol)
{
    if (!plan || plan->live_inputs || !out || !symbol_valid(symbol) ||
        plan->input_count > UINT32_MAX ||
        plan->node_count > (SIZE_MAX - 6)/8 ||
        plan->input_count > (SIZE_MAX - 6 - plan->node_count*8)/4)
        return RS_INVALID_ARGUMENT;
    /* Validate all indices/ranges before touching the stream. */
    RedstoneState validation = {0};
    RsResult result = redstone_state_build(plan, &validation);
    redstone_state_destroy(&validation);
    if (result != RS_OK) return result;
    Query *queries = NULL;
    uint32_t *mapping = NULL, *buckets = NULL;
    CReads reads = {0};
    size_t *read_offsets = NULL;
    RedstoneNodeId *read_targets = NULL;
    size_t query_count = 0, bucket_count = 1;
    if (plan->node_count > SIZE_MAX/sizeof(*queries)) return RS_OUT_OF_MEMORY;
    if (plan->node_count) {
        while (bucket_count < plan->node_count) {
            if (bucket_count > SIZE_MAX/2) return RS_OUT_OF_MEMORY;
            bucket_count *= 2;
        }
        if (bucket_count > SIZE_MAX/sizeof(*buckets)) return RS_OUT_OF_MEMORY;
        queries = calloc(plan->node_count, sizeof(*queries));
        mapping = malloc(plan->node_count*sizeof(*mapping));
        buckets = malloc(bucket_count*sizeof(*buckets));
        reads.seen = malloc(plan->node_count * sizeof(*reads.seen));
        if (!queries || !mapping || !buckets || !reads.seen) {
            result = RS_OUT_OF_MEMORY; goto cleanup;
        }
        for (size_t i = 0; i < bucket_count; ++i) buckets[i] = UINT32_MAX;
        for (size_t i = 0; i < plan->node_count; ++i) reads.seen[i] = UINT32_MAX;
    }
    /* Share identical code, never state. Each body uses constant offsets from
     * its target block. Full text comparison makes hash collisions harmless. */
    for (size_t i = 0; i < plan->node_count; ++i) {
        mapping[i] = UINT32_MAX;
        if (!node_supported(plan, i)) continue;
        CText body = {.reads = &reads, .target = (RedstoneNodeId)i};
        emit_body(&body, plan, i);
        if (body.error != RS_OK) {
            result = body.error;
            free(body.data);
            goto cleanup;
        }
        uint64_t hash = text_hash(&body);
        size_t bucket = (size_t)hash & (bucket_count - 1);
        uint32_t match = buckets[bucket];
        while (match != UINT32_MAX) {
            Query *query = &queries[match];
            if (query->hash == hash && query->body.length == body.length &&
                memcmp(query->body.data, body.data, body.length) == 0) break;
            match = query->next;
        }
        if (match != UINT32_MAX) free(body.data);
        else {
            match = (uint32_t)query_count++;
            queries[match] = (Query){body, hash, buckets[bucket], i};
            buckets[bucket] = match;
        }
        mapping[i] = match;
    }
    read_offsets = calloc(plan->node_count + 1, sizeof(*read_offsets));
    if (reads.count) read_targets = malloc(reads.count * sizeof(*read_targets));
    if (!read_offsets || (reads.count && !read_targets)) {
        result = RS_OUT_OF_MEMORY; goto cleanup;
    }
    for (size_t i = 0; i < reads.count; ++i)
        ++read_offsets[reads.items[i].source + 1];
    for (size_t i = 0; i < plan->node_count; ++i) {
        read_offsets[i + 1] += read_offsets[i];
        reads.seen[i] = 0;
    }
    for (size_t i = 0; i < reads.count; ++i) {
        CRead read = reads.items[i];
        read_targets[read_offsets[read.source] + reads.seen[read.source]++] = read.target;
    }
    fprintf(out, "/* Generated by mcmux. Re-export after changing the core or topology.\n"
        " * Pure queries only: time, writes and notifications use the core runtime.\n"
        " * Identical relative queries share code; all blocks keep independent state. */\n"
        "#include \"rs/compiled.h\"\n\n"
        "static const uint32_t %s_topology[] = {", symbol);
    Topology sink = {.output = out};
    topology(plan, &sink);
    fputs("\n};\n", out);
    for (size_t i = 0; i < query_count; ++i) {
        RedstonePlanNode node = plan->nodes[queries[i].first_node];
        fprintf(out, "\n/* Query shape first seen at (%d, %d, %d). */\n"
            "static RsResult %s_query_%zu(const BlockState *blocks, SimCEvaluation *output)\n{\n",
            node.pos.x, node.pos.y, node.pos.z, symbol, i);
        fputs(queries[i].body.data, out);
        fputs("}\n", out);
    }
    fprintf(out, "\nstatic RsResult %s_evaluate(const BlockState *blocks, "
        "uint32_t node, SimCEvaluation *output)\n{\n", symbol);
    if (plan->node_count) {
        fputs("    static RsResult (*const queries[])(const BlockState *, SimCEvaluation *) = {\n", out);
        for (size_t i = 0; i < plan->node_count; ++i) {
            if (mapping[i] != UINT32_MAX)
                fprintf(out, "        %s_query_%" PRIu32 ",\n", symbol, mapping[i]);
            else fputs("        NULL,\n", out);
        }
        fprintf(out, "    };\n    if (!blocks || !output || node >= %zu) return RS_INVALID_ARGUMENT;\n"
            "    return queries[node] ? queries[node](blocks + node, output) : RS_UNSUPPORTED;\n",
            plan->node_count);
    } else {
        fputs("    (void)blocks; (void)node; (void)output;\n    return RS_INVALID_ARGUMENT;\n", out);
    }
    fprintf(out, "}\n\nstatic const size_t %s_read_offsets[] = {", symbol);
    for (size_t i = 0; i <= plan->node_count; ++i) {
        if (i % 8 == 0) fputs("\n    ", out);
        fprintf(out, "%zu,", read_offsets[i]);
    }
    fputs("\n};\n", out);
    if (reads.count) {
        fprintf(out, "\nstatic const uint32_t %s_read_targets[] = {", symbol);
        for (size_t i = 0; i < reads.count; ++i) {
            if (i % 8 == 0) fputs("\n    ", out);
            fprintf(out, "UINT32_C(%" PRIu32 "),", read_targets[i]);
        }
        fputs("\n};\n", out);
    }
    fprintf(out, "\nconst SimCModel %s = {\n"
        "    .abi_version = SIM_C_ABI_VERSION, .node_count = %zu,\n"
        "    .topology_word_count = %zu, .topology = %s_topology,\n"
        "    .evaluate = %s_evaluate, .read_count = %zu,\n"
        "    .read_offsets = %s_read_offsets, .read_targets = ",
        symbol, plan->node_count, sink.count, symbol, symbol, reads.count, symbol);
    if (reads.count) fprintf(out, "%s_read_targets", symbol);
    else fputs("NULL", out);
    fputs("\n};\n", out);
    result = ferror(out) ? RS_IO_ERROR : RS_OK;
cleanup:
    for (size_t i = 0; i < query_count; ++i) free(queries[i].body.data);
    free(queries); free(mapping); free(buckets);
    free(reads.items); free(reads.seen); free(read_offsets); free(read_targets);
    return result;
}
