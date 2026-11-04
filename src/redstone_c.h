#ifndef RS_REDSTONE_C_H
#define RS_REDSTONE_C_H
#include "redstone_eval.h"

bool redstone_c_matches(const RedstonePlan *plan, const SimCModel *model);
RsResult redstone_c_write(const RedstonePlan *plan, FILE *output,
                          const char *symbol);
#endif
