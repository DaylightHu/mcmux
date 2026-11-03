#include "redstone_queue.h"

#include <stdlib.h>

void redstone_dirty_queue_init(RedstoneDirtyQueue *queue)
{
    if (queue) *queue = (RedstoneDirtyQueue){0};
}

void redstone_dirty_queue_destroy(RedstoneDirtyQueue *queue)
{
    if (!queue) return;
    free(queue->items);
    free(queue->queued);
    *queue = (RedstoneDirtyQueue){0};
}

RsResult redstone_dirty_queue_prepare(RedstoneDirtyQueue *queue,
                                      size_t node_count)
{
    if (!queue || node_count > UINT32_MAX ||
        node_count > SIZE_MAX/sizeof(*queue->items))
        return RS_INVALID_ARGUMENT;
    if (node_count == queue->capacity &&
        (node_count == 0 || (queue->items && queue->queued))) {
        redstone_dirty_queue_clear(queue);
        return RS_OK;
    }

    RedstoneNodeId *items = NULL;
    uint8_t *queued = NULL;
    if (node_count) {
        items = malloc(node_count*sizeof(*items));
        queued = calloc(node_count, sizeof(*queued));
        if (!items || !queued) {
            free(items);
            free(queued);
            return RS_OUT_OF_MEMORY;
        }
    }
    free(queue->items);
    free(queue->queued);
    *queue = (RedstoneDirtyQueue){
        .items = items,
        .queued = queued,
        .capacity = node_count
    };
    return RS_OK;
}

static size_t next_slot(const RedstoneDirtyQueue *queue, size_t slot)
{
    return slot + 1 == queue->capacity ? 0 : slot + 1;
}

void redstone_dirty_queue_clear(RedstoneDirtyQueue *queue)
{
    if (!queue) return;
    while (queue->count) {
        RedstoneNodeId node = queue->items[queue->head];
        if (node < queue->capacity) queue->queued[node] = 0;
        queue->head = next_slot(queue, queue->head);
        --queue->count;
    }
    queue->head = queue->tail = 0;
}

RsResult redstone_dirty_queue_push(RedstoneDirtyQueue *queue,
                                   RedstoneNodeId node)
{
    if (!queue || node >= queue->capacity || !queue->items || !queue->queued)
        return RS_INVALID_ARGUMENT;
    if (queue->queued[node]) return RS_UNCHANGED;
    if (queue->count == queue->capacity) return RS_INTERNAL_ERROR;
    queue->items[queue->tail] = node;
    queue->tail = next_slot(queue, queue->tail);
    ++queue->count;
    queue->queued[node] = 1;
    return RS_OK;
}

bool redstone_dirty_queue_pop(RedstoneDirtyQueue *queue,
                              RedstoneNodeId *node)
{
    if (!queue || !node || !queue->count || !queue->items || !queue->queued)
        return false;
    RedstoneNodeId current = queue->items[queue->head];
    if (current >= queue->capacity || !queue->queued[current]) return false;
    queue->head = next_slot(queue, queue->head);
    --queue->count;
    queue->queued[current] = 0;
    *node = current;
    return true;
}

RsResult redstone_dirty_queue_push_all(RedstoneDirtyQueue *queue)
{
    if (!queue || (queue->capacity && (!queue->items || !queue->queued)))
        return RS_INVALID_ARGUMENT;
    for (size_t node = 0; node < queue->capacity; ++node) {
        RsResult result = redstone_dirty_queue_push(
            queue, (RedstoneNodeId)node);
        if (result != RS_OK && result != RS_UNCHANGED) return result;
    }
    return RS_OK;
}

RsResult redstone_dirty_queue_push_consumers(RedstoneDirtyQueue *queue,
                                             const RedstonePlan *plan,
                                             RedstoneNodeId source)
{
    if (!queue || !plan ||
        (queue->capacity && (!queue->items || !queue->queued)) ||
        (plan->node_count && !plan->nodes) ||
        queue->capacity != plan->node_count ||
        source >= plan->node_count ||
        (plan->consumer_count && !plan->consumers))
        return RS_INVALID_ARGUMENT;
    RedstonePlanNode node = plan->nodes[source];
    if (node.first_consumer > plan->consumer_count ||
        node.consumer_count > plan->consumer_count - node.first_consumer)
        return RS_INTERNAL_ERROR;
    for (size_t offset = 0; offset < node.consumer_count; ++offset)
        if (plan->consumers[node.first_consumer + offset].target >=
            plan->node_count) return RS_INTERNAL_ERROR;
    for (size_t offset = 0; offset < node.consumer_count; ++offset) {
        RedstoneNodeId target =
            plan->consumers[node.first_consumer + offset].target;
        RsResult result = redstone_dirty_queue_push(queue, target);
        if (result != RS_OK && result != RS_UNCHANGED) return result;
    }
    return RS_OK;
}
