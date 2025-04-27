#ifndef __AECP_AEM_COUNTER_H__
#define __AECP_AEM_COUNTER_H__

#define AECP_AEM_COUNTER_BLOCK_QUADLET_COUNT                32

/* Milan v1.2 Table 5.13 - 5.14 */
#define AECP_AEM_COUNTER_AVB_IF_LINK_UP                     0
#define AECP_AEM_COUNTER_AVB_IF_LINK_DOWN                   1
#define AECP_AEM_COUNTER_AVB_IF_GPTP_GM_CH                  5
#define AECP_AEM_COUNTER_AVB_IF_FRAME_TX                    2
#define AECP_AEM_COUNTER_AVB_IF_FRAME_RX                    3
#define AECP_AEM_COUNTER_AVB_IF_RX_CRC_ERROR                4

/* Milan v1.2 Table 5.15 */
#define AECP_AEM_COUNTER_CLK_DOMAIN_LOCKED                  0
#define AECP_AEM_COUNTER_CLK_DOMAIN_UNLOCKED                1

/* Milan v1.2 Table 5.16 */
#define AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_LOCKED          0
#define AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_UNLOCKED        1
#define AECP_AEM_COUNTER_STREAM_INPUT_STREAM_INTERRUPTED    2
#define AECP_AEM_COUNTER_STREAM_INPUT_SEQ_NUM_MISMATCH      3
#define AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_RESET           4
#define AECP_AEM_COUNTER_STREAM_INPUT_TIMESTAMP_UNCERTAIN   5
#define AECP_AEM_COUNTER_STREAM_INPUT_UNSUPPORTED_FORMAT    8
#define AECP_AEM_COUNTER_STREAM_INPUT_LATE_TIMESTAMP        9
#define AECP_AEM_COUNTER_STREAM_INPUT_EARLY_TIMESTAMP       10
#define AECP_AEM_COUNTER_STREAM_INPUT_FRAME_RX              11

/* Milan v1.2 Table 5.17 */
#define AECP_AEM_COUNTER_STREAM_OUT_STREAM_START            0
#define AECP_AEM_COUNTER_STREAM_OUT_STREAM_STOP             1
#define AECP_AEM_COUNTER_STREAM_OUT_MEDIA_RESET             2
#define AECP_AEM_COUNTER_STREAM_OUT_TIMESTAMP_UNCERTAIN     3
#define AECP_AEM_COUNTER_STREAM_OUT_FRAME_TX                4

/**
 * Retieve the mask from thje bit above as described in
 * 1722.1-2021 Clause 7.4.42
 */
#define AECP_AEM_COUNTER_GET_MASK(x)                        (1 << x)

/**
 *  Retrieve the position within the buffer as described in the specification
 *  1722.1-2021 Clause 7.4.42
 */
#define AECP_AEM_COUNTER_GET_BUF_POS(x)                     (x << 2)


#endif // __AECP_AEM_COUNTER_H__