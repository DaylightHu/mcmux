#ifndef MC_REGION_EDIT_INTERNAL_H
#define MC_REGION_EDIT_INTERNAL_H

#include "rs/region_edit.h"

RsResult region_editor_copy_with_options(RegionEditor *editor, const BlockPos *anchor,
                                        bool skip_air);
RsResult region_editor_stack_with_options(RegionEditor *editor, uint32_t copies,
                                         Direction direction, bool skip_air, size_t *changed);
RsResult region_editor_move_with_options(RegionEditor *editor, uint32_t distance,
                                        Direction direction, bool skip_air, size_t *changed);

/* Apply a complete source-region snapshot to a destination editor. This is
 * an internal bridge for structure writeback; it is not part of the public
 * editor API. */
RsResult region_editor_transfer(RegionEditor *source, RegionEditor *target,
                                BlockPos first, BlockPos second,
                                BlockPos anchor, bool skip_air,
                                size_t *changed);

#endif
