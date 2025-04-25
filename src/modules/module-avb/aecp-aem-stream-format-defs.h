#ifndef __AECP_AEM_STREAM_FORMATS_DEFS_H__
#define __AECP_AEM_STREAM_FORMATS_DEFS_H__

#include "utils.h"

// 1722.1-2021 Clause 7.3.2
#define AECP_AEM_STREAM_FORMAT_VENDOR_FLAG              (1)
#define AECP_AEM_STREAM_FORMAT_VENDOR_SHIFT(x)          (((uint64_t)x&(0x1))<<63)

// 1722.1-2021 Table 7.65
#define AECP_AEM_STREAM_FORMAT_SUBTYPE_AVTP             (2)
#define AECP_AEM_STREAM_FORMAT_SUBTYPE_SHIFT(x)         (((uint64_t)x&(0xff))<<56)

// 1722.1-2021 Table 7.66
#define AECP_AEM_STREAM_FORMAT_NOMINAL_SR_UNSPECIFIED   (0)
#define AECP_AEM_STREAM_FORMAT_NOMINAL_SR_8KHZ          (1)
#define AECP_AEM_STREAM_FORMAT_NOMINAL_SR_16KHZ         (2)
#define AECP_AEM_STREAM_FORMAT_NOMINAL_SR_32KHZ         (3)
#define AECP_AEM_STREAM_FORMAT_NOMINAL_SR_44_1KHZ       (4)
#define AECP_AEM_STREAM_FORMAT_NOMINAL_SR_48KHZ         (5)
#define AECP_AEM_STREAM_FORMAT_NOMINAL_SR_88_2KHZ       (6)
#define AECP_AEM_STREAM_FORMAT_NOMINAL_SR_96KHZ         (7)
#define AECP_AEM_STREAM_FORMAT_NOMINAL_SR_176_4KHZ      (8)
#define AECP_AEM_STREAM_FORMAT_NOMINAL_SR_192KHZ        (9)

#define AECP_AEM_STREAM_FORMAT_NOMINAL_SR_SHIFT(x)      (((uint64_t)x&(0xf))<<48)

// 1722.1-2021 Table 7.67

#define AECP_AEM_STREAM_FORMAT_VALUES_UNSPECIFIED              (0)
#define AECP_AEM_STREAM_FORMAT_VALUES_FLOAT                    (1)
#define AECP_AEM_STREAM_FORMAT_VALUES_32BIT_INT                (2)
#define AECP_AEM_STREAM_FORMAT_VALUES_24BITPACKTED_INT         (3)
#define AECP_AEM_STREAM_FORMAT_VALUES_16BIT_INT                (4)
#define AECP_AEM_STREAM_FORMAT_VALUES_SHIFT(x)                 (((uint64_t)x&(0xff))<<40)

// Bit depth
#define AECP_AEM_STREAM_FORMAT_BIT_DEPTH(x)                    (((uint64_t)x&(0xff))<<32)

// Channel per frames
#define AECP_AEM_STREAM_FORMAT_CPF_SHIFT(x)                    (((uint64_t)x&(0x3ff))<<22)

// Sample per frames
#define AECP_AEM_STREAM_FORMAT_SPF_SHIFT(x)                    (((uint64_t)x&(0x3ff))<<12)

#define AECP_AEM_HELPER_FORMAT_STD_AVTP(freq, values, depth, cpf, spf)           \
(uint64_t)                                                                       \
AECP_AEM_STREAM_FORMAT_SUBTYPE_SHIFT(AECP_AEM_STREAM_FORMAT_SUBTYPE_AVTP) |      \
AECP_AEM_STREAM_FORMAT_NOMINAL_SR_SHIFT(AECP_AEM_STREAM_FORMAT_NOMINAL_SR_## freq )|\
AECP_AEM_STREAM_FORMAT_VALUES_SHIFT(AECP_AEM_STREAM_FORMAT_VALUES_ ## values) |    \
AECP_AEM_STREAM_FORMAT_BIT_DEPTH(depth)|                                         \
AECP_AEM_STREAM_FORMAT_CPF_SHIFT(cpf) |                                          \
AECP_AEM_STREAM_FORMAT_SPF_SHIFT(spf)

#endif //__AECP_AEM_STREAM_FORMATS_DEFS_H__