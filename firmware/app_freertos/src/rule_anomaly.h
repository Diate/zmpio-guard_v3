#ifndef RULE_ANOMALY_H
#define RULE_ANOMALY_H

#include "fpga_dsp_hal.h"
#include "zlog_feature_v2.h"

/*
 * Stateless, task-context-only rule evaluation over one feature_frame_v2.
 * Called from fpga_result_task (main.c) right after
 * fpga_dsp_hal_pop_feature(). That task never runs in ISR context, which is
 * a requirement for this module, and is why it needs no task or queue of its
 * own.
 */
void rule_anomaly_evaluate(const fpga_feature_frame_t *frame,
                           zlog_anomaly_rule_v1_t *out);

#endif
