#ifndef RS_REDSTONE_QUEUE_H
#define RS_REDSTONE_QUEUE_H

#include "redstone_plan.h"

/* Fixed-capacity FIFO for executor work. A node can appear at most once while
 * pending; popping it clears the mark so a later upstream change can enqueue
 * it again. Capacity is the plan's node count, so pushes never allocate. */
typedef struct {
    RedstoneNodeId *items;
    uint8_t *queued;
    size_t capacity, head, tail, count;
} RedstoneDirtyQueue;

void redstone_dirty_queue_init(RedstoneDirtyQueue *queue);
void redstone_dirty_queue_destroy(RedstoneDirtyQueue *queue);
RsResult redstone_dirty_queue_prepare(RedstoneDirtyQueue *queue,
                                      size_t node_count);
void redstone_dirty_queue_clear(RedstoneDirtyQueue *queue);
RsResult redstone_dirty_queue_push(RedstoneDirtyQueue *queue,
                                   RedstoneNodeId node);
bool redstone_dirty_queue_pop(RedstoneDirtyQueue *queue,
                              RedstoneNodeId *node);
RsResult redstone_dirty_queue_push_all(RedstoneDirtyQueue *queue);
RsResult redstone_dirty_queue_push_consumers(RedstoneDirtyQueue *queue,
                                             const RedstonePlan *plan,
                                             RedstoneNodeId source);

#endif
