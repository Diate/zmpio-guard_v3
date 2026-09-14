#include "rule_anomaly.h"

#include "app_config.h"

void rule_anomaly_evaluate(const fpga_feature_frame_t *frame,
                           zlog_anomaly_rule_v1_t *out)
{
    out->rule_version = (uint8_t)APP_RULE_ANOMALY_VERSION;
    out->metric_id = (uint8_t)ZLOG_RULE_METRIC_RMS;
    out->frame_sequence = frame->frame_sequence;
    out->value_q24_8 = frame->rms_q24_8;
    out->threshold_q24_8 = (uint32_t)APP_RULE_ANOMALY_RMS_THRESHOLD_Q24_8;
    out->verdict = (frame->rms_q24_8 > out->threshold_q24_8) ? 1U : 0U;
    out->reserved0 = 0U;
}
