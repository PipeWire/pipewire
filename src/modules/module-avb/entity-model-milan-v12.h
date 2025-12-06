/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Simon Gapp <simon.gapp@kebag-logic.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef __DESCRIPTOR_ENTITY_MODEL_MILAN_H__
#define __DESCRIPTOR_ENTITY_MODEL_MILAN_H__

#define TALKER_ENABLE 1

// TODO: Make defines as long as specified length
/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.12 - STRINGS Descriptor
* Up to 7 localized strings
*/
#define DSC_STRINGS_0_DEVICE_NAME "PipeWire"
#define DSC_STRINGS_1_CONFIGURATION_NAME "NON - redundant - 48kHz"
#define DSC_STRINGS_2_MANUFACTURER_NAME "Kebag Logic"
#define DSC_STRINGS_3_GROUP_NAME "Kebag Logic"
#define DSC_STRINGS_4_MAINTAINER_0 "Alexandre Malki"
#define DSC_STRINGS_4_MAINTAINER_1 "Simon Gapp"

/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.11 - LOCALE Descriptor */
#define DSC_LOCALE_LANGUAGE_CODE "en-EN"
#define DSC_LOCALE_NO_OF_STRINGS 1
#define DSC_LOCALE_BASE_STRINGS 0

/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.1 - ENTITY Descriptor */
/* Milan v1.2, Sec. 5.3.3.1 */

#define DSC_ENTITY_MODEL_ENTITY_ID 0xDEAD00BEEF00FEED
#define DSC_ENTITY_MODEL_ID 0
#define DSC_ENTITY_MODEL_ENTITY_CAPABILITIES (AVB_ADP_ENTITY_CAPABILITY_AEM_SUPPORTED | \
        AVB_ADP_ENTITY_CAPABILITY_CLASS_A_SUPPORTED | \
        AVB_ADP_ENTITY_CAPABILITY_GPTP_SUPPORTED | \
        AVB_ADP_ENTITY_CAPABILITY_AEM_IDENTIFY_CONTROL_INDEX_VALID | \
        AVB_ADP_ENTITY_CAPABILITY_AEM_INTERFACE_INDEX_VALID)
/* IEEE 1722.1-2021, Table 7-2 - ENTITY Descriptor
* This is the maximum number of STREAM_OUTPUT
* descriptors the ATDECC Entity has for
* Output Streams in any of its Configurations */

#if TALKER_ENABLE
    #define DSC_ENTITY_MODEL_TALKER_STREAM_SOURCES 8
    #define DSC_ENTITY_MODEL_TALKER_CAPABILITIES (AVB_ADP_TALKER_CAPABILITY_IMPLEMENTED | \
            AVB_ADP_TALKER_CAPABILITY_AUDIO_SOURCE)
#else
    #define DSC_ENTITY_MODEL_TALKER_STREAM_SOURCES 0
    #define DSC_ENTITY_MODEL_TALKER_CAPABILITIES 0
#endif


#define DSC_ENTITY_MODEL_LISTENER_STREAM_SINKS 8
#define DSC_ENTITY_MODEL_LISTENER_CAPABILITIES (AVB_ADP_LISTENER_CAPABILITY_IMPLEMENTED | \
        AVB_ADP_LISTENER_CAPABILITY_AUDIO_SINK)
#define DSC_ENTITY_MODEL_CONTROLLER_CAPABILITIES 0
/* IEEE 1722.1-2021, Table 7-2 ENTITY Descriptor
* The available index of the ATDECC Entity.
* This is the same as the available_index field
* in ATDECC Discovery Protocol.*/
#define DSC_ENTITY_MODEL_AVAILABLE_INDEX 0
/* IEEE 1722.1-2021, Table 7-2 ENTITY Descriptor
    * The association ID for the ATDECC Entity.
    * This is the same as association_id field
    * in ATDECC Discovery Protocol*/
#define DSC_ENTITY_MODEL_ASSOCIATION_ID 0
#define DSC_ENTITY_MODEL_ENTITY_NAME DSC_STRINGS_0_DEVICE_NAME
/* IEEE 1722.1-2021, Table 7-2 - ENTITY Descriptor
    * The localized string reference pointing to the
    * localized vendor name. See 7.3.7. */
#define DSC_ENTITY_MODEL_VENDOR_NAME_STRING 2
/* IEEE 1722.1-2021, Table 7-2 - ENTITY Descriptor
    * The localized string reference pointing to the
    * localized model name. See 7.3.7. */
#define DSC_ENTITY_MODEL_MODEL_NAME_STRING 0
#define DSC_ENTITY_MODEL_FIRMWARE_VERSION "0.3.48"
#define DSC_ENTITY_MODEL_GROUP_NAME DSC_STRINGS_3_GROUP_NAME
#define DSC_ENTITY_MODEL_SERIAL_NUMBER "0xBEBEDEAD"
#define DSC_ENTITY_MODEL_CONFIGURATIONS_COUNT 2
#define DSC_ENTITY_MODEL_CURRENT_CONFIGURATION 0

/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.2 - CONFIGURATION Descriptor*/
/* Milan v1.2, Sec. 5.3.3.2 */
#define DSC_CONFIGURATION_DESCRIPTOR_COUNTS_COUNT 8
#define DSC_CONFIGURATION_OBJECT_NAME DSC_STRINGS_1_CONFIGURATION_NAME
/* IEEE 1722.1-2021, Table 7-3 CONFIGURATION Descriptor
    * The localized string reference pointing to the
    * localized Configuration name. */
#define DSC_CONFIGURATION_LOCALIZED_DESCRIPTION 1
/* IEEE 1722.1-2021, Table 7-3 CONFIGURATION Descriptor
    * The offset to the descriptor_counts field from the
    * start of the descriptor. This field is set to 74 for
    * this version of AEM. */
#define DSC_CONFIGURATION_DESCRIPTOR_COUNTS_OFFSET 74

#define DSC_CONFIGURATION_NO_OF_AUDIO_UNITS 1
#define DSC_CONFIGURATION_NO_OF_STREAM_INPUTS 2

#define DSC_CONFIGURATION_NO_OF_STREAM_OUTPUTS 1

#define DSC_CONFIGURATION_NO_OF_AVB_INTERFACES 1
#define DSC_CONFIGURATION_NO_OF_CLOCK_DOMAINS 1
#define DSC_CONFIGURATION_NO_OF_CLOCK_SOURCES 3
#define DSC_CONFIGURATION_NO_OF_CONTROLS 1
#define DSC_CONFIGURATION_NO_OF_LOCALES 1

/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.22 CONTROL Descriptor*/
/* Milan v1.2, Sec. 5.3.3.10 */

#define DSC_CONTROL_OBJECT_NAME "Identify"
#define DSC_CONTROL_LOCALIZED_DESCRIPTION AVB_AEM_DESC_INVALID
#define DSC_CONTROL_BLOCK_LATENCY 500
#define DSC_CONTROL_CONTROL_LATENCY 500
#define DSC_CONTROL_CONTROL_DOMAIN 0
#define DSC_CONTROL_CONTROL_VALUE_TYPE AECP_AEM_CTRL_LINEAR_UINT8
#define DSC_CONTROL_CONTROL_TYPE AEM_CTRL_TYPE_IDENTIFY
/* IEEE 1722.1-2021, Table 7-39 - CONTROL Descriptor
    * The time period in microseconds from when a control
    * is set with the SET_CONTROL command till it automatically
    * resets to its default values.
    * When this is set to zero (0) automatic resets do not happen. */
// TODO: Milan v1.2: The PAAD remains in identification mode until the value of the “IDENTIFY” CONTROL descriptor is set back to 0.
#define DSC_CONTROL_RESET_TIME 3
#define DSC_CONTROL_NUMBER_OF_VALUES 1
#define DSC_CONTROL_SIGNAL_TYPE AVB_AEM_DESC_INVALID
#define DSC_CONTROL_SIGNAL_INDEX 0
#define DSC_CONTROL_SIGNAL_OUTPUT 0

#define DSC_CONTROL_IDENTIFY_MIN 0
#define DSC_CONTROL_IDENTIFY_MAX 255
#define DSC_CONTROL_IDENTIFY_STEP 255
#define DSC_CONTROL_IDENTIFY_DEFAULT_VALUE 0
#define DSC_CONTROL_IDENTIFY_CURRENT_VALUE 0
#define DSC_CONTROL_LOCALIZED_DESCRIPTION AVB_AEM_DESC_INVALID

/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.19 AUDIO_MAP Descriptor */
/* Milan v1.2, Sec. 5.3.3.9 */

// TODO: Prepared for for loop over total number of audio maps
#define DSC_AUDIO_MAPS_TOTAL_NO_OF_MAPS 2

#define DSC_AUDIO_MAPS_NO_OF_MAPPINGS 8
#define DSC_AUDIO_MAPS_MAPPING_STREAM_INDEX 0
#define DSC_AUDIO_MAPS_MAPPING_CLUSTER_CHANNEL 0

/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.16 AUDIO_CLUSTER Descriptor */
/* Milan v1.2, Sec. 5.3.3.8 */
#define DSC_AUDIO_CLUSTER_NO_OF_CLUSTERS 16
#define DSC_AUDIO_CLUSTER_OBJECT_NAME_LEN_IN_OCTET 64
#define DSC_AUDIO_CLUSTER_OBJECT_NAME_INPUT "Input"
#define DSC_AUDIO_CLUSTER_OBJECT_NAME_OUTPUT "Output"

#define DSC_AUDIO_CLUSTER_LOCALIZED_DESCRIPTION AVB_AEM_DESC_INVALID

/* The signal_type and signal_index fields indicate the
    * object providing the signal destined for the channels
    * of the streams mapped to the port. For a signal which
    * is sourced internally from the Unit, the signal_type
    * is set to AUDIO_UNIT and signal_index is set to the
    * index of the Unit. For a Cluster attached to a
    * STREAM_PORT_INPUT the signal_type and signal_index
    * fields is set to INVALID and zero (0) respectively. */
#define DSC_AUDIO_CLUSTER_SIGNAL_TYPE 0
#define DSC_AUDIO_CLUSTER_SIGNAL_INDEX 0
/* The index of the output of the signal source of the
    * cluster. For a signal_type of SIGNAL_SPLITTER or
    * SIGNAL_DEMULTIPLEXER this is which output of the
    * object it is being sourced from, for a signal_type
    * of MATRIX this is the column the signal is from
    * and for any other signal_type this is zero (0). */
#define DSC_AUDIO_CLUSTER_SIGNAL_OUTPUT 0
#define DSC_AUDIO_CLUSTER_PATH_LATENCY_IN_NS 500
#define DSC_AUDIO_CLUSTER_BLOCK_LATENCY_IN_NS 500
#define DSC_AUDIO_CLUSTER_CHANNEL_COUNT 1
#define DSC_AUDIO_CLUSTER_FORMAT AVB_AEM_AUDIO_CLUSTER_TYPE_MBLA
#define DSC_AUDIO_CLUSTER_AES3_DATA_TYPE_REF 0
#define DSC_AUDIO_CLUSTER_AES3_DATA_TYPE 0

/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.13 STREAM_PORT_INPUT Descriptor */
/* Milan v1.2, Sec. 5.3.3.7 */

#define DSC_STREAM_PORT_INPUT_CLOCK_DOMAIN_INDEX 0x0000
#define DSC_STREAM_PORT_INPUT_PORT_FLAGS AVB_AEM_PORT_FLAG_CLOCK_SYNC_SOURCE
/* The number of clusters within the Port. This corresponds to the number of
    * AUDIO_CLUSTER, VIDEO_CLUSTER or SENSOR_CLUSTER descriptors which represent
    * these clusters. */
// TODO: Validate value
#define DSC_STREAM_PORT_INPUT_NUMBER_OF_CONTROLS 0
#define DSC_STREAM_PORT_INPUT_BASE_CONTROL 0
// TODO: Validate value
#define DSC_STREAM_PORT_INPUT_NUMBER_OF_CLUSTERS 8
#define DSC_STREAM_PORT_INPUT_BASE_CLUSTER 0
#define DSC_STREAM_PORT_INPUT_NUMBER_OF_MAPS 1
#define DSC_STREAM_PORT_INPUT_BASE_MAP 0

    /**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.13 STREAM_PORT_OUTPUT Descriptor */
/* Milan v1.2, Sec. 5.3.3.7 */

#define DSC_STREAM_PORT_OUTPUT_CLOCK_DOMAIN_INDEX 0
#define DSC_STREAM_PORT_OUTPUT_PORT_FLAGS AVB_AEM_PORT_FLAG_NO_FLAG
#define DSC_STREAM_PORT_OUTPUT_NUMBER_OF_CONTROLS 0
#define DSC_STREAM_PORT_OUTPUT_BASE_CONTROL 0
// TODO: Verify
#define DSC_STREAM_PORT_OUTPUT_NUMBER_OF_CLUSTERS 8
#define DSC_STREAM_PORT_OUTPUT_BASE_CLUSTER 8
#define DSC_STREAM_PORT_OUTPUT_NUMBER_OF_MAPS 1
#define DSC_STREAM_PORT_OUTPUT_BASE_MAP 1

/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.3 AUDIO_UNIT Descriptor */
/* Milan v1.2, Sec. 5.3.3.3 */

/* IEEE 1722.1-2021, Sec. 7.3.1
    * A sampling rate consists of a 3 bit pull field
    * representing a multiplier and a 29 bit
    * base_frequency in hertz, as detailed in Figure 7-2.
    * The pull field specifies the multiplier modifier
    * of the base_frequency field which is required to
    * calculate the appropriate nominal sampling rate.
    * The pull field may have one of the values defined
    * in Table 7-70:
    * The base_frequency field defines the nominal base
    * sampling rate in Hz, from 1 Hz to 536 870 911 Hz.
    * The value of this field is augmented by the
    * pull field value.*/
#define BUILD_SAMPLING_RATE(pull, base_freq_hz) \
(((uint32_t)(pull) << 29) | ((uint32_t)(base_freq_hz) & 0x1FFFFFFF))


#define DSC_AUDIO_UNIT_OBJECT_NAME                          ""
#define DSC_AUDIO_UNIT_LOCALIZED_DESCRIPTION                0xFFFF
#define DSC_AUDIO_UNIT_CLOCK_DOMAIN_INDEX                   0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_STREAM_INPUT_PORTS         0x0001
#define DSC_AUDIO_UNIT_BASE_STREAM_INPUT_PORT               0x0000

#if TALKER_ENABLE
    #define DSC_AUDIO_UNIT_NUMBER_OF_STREAM_OUTPUT_PORTS    0x0001
#else
    #define DSC_AUDIO_UNIT_NUMBER_OF_STREAM_OUTPUT_PORTS    0x0000
#endif

#define DSC_AUDIO_UNIT_BASE_STREAM_OUTPUT_PORT              0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_EXTERNAL_INPUT_PORTS       0x0008
#define DSC_AUDIO_UNIT_BASE_EXTERNAL_INPUT_PORT             0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_EXTERNAL_OUTPUT_PORTS      0x0008
#define DSC_AUDIO_UNIT_BASE_EXTERNAL_OUTPUT_PORT            0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_INTERNAL_INPUT_PORTS       0x0000
#define DSC_AUDIO_UNIT_BASE_INTERNAL_INPUT_PORT             0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_INTERNAL_OUTPUT_PORTS      0x0000
#define DSC_AUDIO_UNIT_BASE_INTERNAL_OUTPUT_PORT            0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_CONTROLS                   0x0000
#define DSC_AUDIO_UNIT_BASE_CONTROL                         0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_SIGNAL_SELECTORS           0x0000
#define DSC_AUDIO_UNIT_BASE_SIGNAL_SELECTOR                 0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_MIXERS                     0x0000
#define DSC_AUDIO_UNIT_BASE_MIXER                           0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_MATRICES                   0x0000
#define DSC_AUDIO_UNIT_BASE_MATRIX                          0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_SPLITTERS                  0x0000
#define DSC_AUDIO_UNIT_BASE_SPLITTER                        0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_COMBINERS                  0x0000
#define DSC_AUDIO_UNIT_BASE_COMBINER                        0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_DEMULTIPLEXERS             0x0000
#define DSC_AUDIO_UNIT_BASE_DEMULTIPLEXER                   0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_MULTIPLEXERS               0x0000
#define DSC_AUDIO_UNIT_BASE_MULTIPLEXER                     0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_TRANSCODERS                0x0000
#define DSC_AUDIO_UNIT_BASE_TRANSCODER                      0x0000

#define DSC_AUDIO_UNIT_NUMBER_OF_CONTROL_BLOCKS             0x0000
#define DSC_AUDIO_UNIT_BASE_CONTROL_BLOCK                   0x0000

#define DSC_AUDIO_UNIT_SAMPLING_RATE_PULL					0
#define DSC_AUDIO_UNIT_SAMPLING_RATE_BASE_FREQ_IN_HZ		48000
#define DSC_AUDIO_UNIT_CURRENT_SAMPLING_RATE_IN_HZ          \
BUILD_SAMPLING_RATE(DSC_AUDIO_UNIT_SAMPLING_RATE_PULL, DSC_AUDIO_UNIT_SAMPLING_RATE_BASE_FREQ_IN_HZ)
/* The offset to the sample_rates field from the start of the descriptor.
    * This field is 144 for this version of AEM.*/
#define DSC_AUDIO_UNIT_SAMPLING_RATES_OFFSET                144
#define DSC_AUDIO_UNIT_SUPPORTED_SAMPLING_RATE_COUNT        0x0001
#define DSC_AUDIO_UNIT_SUPPORTED_SAMPLING_RATE_IN_HZ_0      \
BUILD_SAMPLING_RATE(DSC_AUDIO_UNIT_SAMPLING_RATE_PULL, DSC_AUDIO_UNIT_SAMPLING_RATE_BASE_FREQ_IN_HZ)

/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.6 STREAM_INPUT Descriptor */
/* Milan v1.2, Sec. 5.3.3.4 */

// TODO: 1722.1 lists redundant parameters that are not mentioned here.

#define DSC_STREAM_INPUT_OBJECT_NAME "Stream 1"
#define DSC_STREAM_INPUT_LOCALIZED_DESCRIPTION AVB_AEM_DESC_INVALID
#define DSC_STREAM_INPUT_CLOCK_DOMAIN_INDEX 0
#define DSC_STREAM_INPUT_STREAM_FLAGS (AVB_AEM_DESC_STREAM_FLAG_SYNC_SOURCE | AVB_AEM_DESC_STREAM_FLAG_CLASS_A)
// To match my talker
// TODO: Define based on AUDIO_UNIT etc.
#define DSC_STREAM_INPUT_CURRENT_FORMAT 0x0205022001006000ULL

// TODO: Is 132 here, should be 138 according to spec
#define DSC_STREAM_INPUT_FORMATS_OFFSET (4 + sizeof(struct avb_aem_desc_stream))
#define DSC_STREAM_INPUT_NUMBER_OF_FORMATS 5

#define DSC_STREAM_INPUT_BACKUP_TALKER_ENTITY_ID_0 0
#define DSC_STREAM_INPUT_BACKUP_TALKER_UNIQUE_ID_0 0

#define DSC_STREAM_INPUT_BACKUP_TALKER_ENTITY_ID_1 0
#define DSC_STREAM_INPUT_BACKUP_TALKER_UNIQUE_ID_1 0

#define DSC_STREAM_INPUT_BACKUP_TALKER_ENTITY_ID_2 0
#define DSC_STREAM_INPUT_BACKUP_TALKER_UNIQUE_ID_2 0

#define DSC_STREAM_INPUT_BACKEDUP_TALKER_ENTITY_ID 0
#define DSC_STREAM_INPUT_BACKEDUP_TALKER_UNIQUE_ID 0

#define DSC_STREAM_INPUT_AVB_INTERFACE_INDEX 0
#define DSC_STREAM_INPUT_BUFFER_LENGTH_IN_NS 2126000

#define DSC_STREAM_INPUT_FORMATS_0 DSC_STREAM_INPUT_CURRENT_FORMAT
#define DSC_STREAM_INPUT_FORMATS_1 0x0205022000406000ULL
#define DSC_STREAM_INPUT_FORMATS_2 0x0205022000806000ULL
#define DSC_STREAM_INPUT_FORMATS_3 0x0205022001806000ULL
#define DSC_STREAM_INPUT_FORMATS_4 0x0205022002006000ULL

/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.6 STREAM_INPUT Descriptor */
/* Milan v1.2, Sec. 5.3.3.4 */

#define DSC_STREAM_INPUT_CRF_OBJECT_NAME "CRF"
#define DSC_STREAM_INPUT_CRF_LOCALIZED_DESCRIPTION AVB_AEM_DESC_INVALID
#define DSC_STREAM_INPUT_CRF_CLOCK_DOMAIN_INDEX 0
#define DSC_STREAM_INPUT_CRF_STREAM_FLAGS (AVB_AEM_DESC_STREAM_FLAG_SYNC_SOURCE | AVB_AEM_DESC_STREAM_FLAG_CLASS_A)
#define DSC_STREAM_INPUT_CRF_CURRENT_FORMAT 0x041060010000BB80ULL

#define DSC_STREAM_INPUT_CRF_FORMATS_OFFSET (4 + sizeof(struct avb_aem_desc_stream))
#define DSC_STREAM_INPUT_CRF_NUMBER_OF_FORMATS 1

#define DSC_STREAM_INPUT_CRF_BACKUP_TALKER_ENTITY_ID_0 0
#define DSC_STREAM_INPUT_CRF_BACKUP_TALKER_UNIQUE_ID_0 0

#define DSC_STREAM_INPUT_CRF_BACKUP_TALKER_ENTITY_ID_1 0
#define DSC_STREAM_INPUT_CRF_BACKUP_TALKER_UNIQUE_ID_1 0

#define DSC_STREAM_INPUT_CRF_BACKUP_TALKER_ENTITY_ID_2 0
#define DSC_STREAM_INPUT_CRF_BACKUP_TALKER_UNIQUE_ID_2 0

#define DSC_STREAM_INPUT_CRF_BACKEDUP_TALKER_ENTITY_ID 0
#define DSC_STREAM_INPUT_CRF_BACKEDUP_TALKER_UNIQUE_ID 0

#define DSC_STREAM_INPUT_CRF_AVB_INTERFACE_INDEX 0
#define DSC_STREAM_INPUT_CRF_BUFFER_LENGTH_IN_NS 2126000

#define DSC_STREAM_INPUT_CRF_FORMATS_0 DSC_STREAM_INPUT_CRF_CURRENT_FORMAT

/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.6 STREAM_OUTPUT Descriptor */
/* Milan v1.2, Sec. 5.3.3.4 */

#define DSC_STREAM_OUTPUT_OBJECT_NAME "Stream output 1"
#define DSC_STREAM_OUTPUT_LOCALIZED_DESCRIPTION AVB_AEM_DESC_INVALID
#define DSC_STREAM_OUTPUT_CLOCK_DOMAIN_INDEX 0
#define DSC_STREAM_OUTPUT_STREAM_FLAGS (AVB_AEM_DESC_STREAM_FLAG_CLASS_A)
#define DSC_STREAM_OUTPUT_CURRENT_FORMAT 0x0205022002006000ULL

#define DSC_STREAM_OUTPUT_FORMATS_OFFSET (4 + sizeof(struct avb_aem_desc_stream))
#define DSC_STREAM_OUTPUT_NUMBER_OF_FORMATS 5

#define DSC_STREAM_OUTPUT_BACKUP_TALKER_ENTITY_ID_0 0
#define DSC_STREAM_OUTPUT_BACKUP_TALKER_UNIQUE_ID_0 0

#define DSC_STREAM_OUTPUT_BACKUP_TALKER_ENTITY_ID_1 0
#define DSC_STREAM_OUTPUT_BACKUP_TALKER_UNIQUE_ID_1 0

#define DSC_STREAM_OUTPUT_BACKUP_TALKER_ENTITY_ID_2 0
#define DSC_STREAM_OUTPUT_BACKUP_TALKER_UNIQUE_ID_2 0

#define DSC_STREAM_OUTPUT_BACKEDUP_TALKER_ENTITY_ID 0
#define DSC_STREAM_OUTPUT_BACKEDUP_TALKER_UNIQUE_ID 0

#define DSC_STREAM_OUTPUT_AVB_INTERFACE_INDEX 0
#define DSC_STREAM_OUTPUT_BUFFER_LENGTH_IN_NS 8

// TODO: No hardcoded values!
#define DSC_STREAM_OUTPUT_FORMATS_0 0x0205022000406000ULL
#define DSC_STREAM_OUTPUT_FORMATS_1 0x0205022000806000ULL
#define DSC_STREAM_OUTPUT_FORMATS_2 0x0205022001006000ULL
#define DSC_STREAM_OUTPUT_FORMATS_3 0x0205022001806000ULL
#define DSC_STREAM_OUTPUT_FORMATS_4 DSC_STREAM_OUTPUT_CURRENT_FORMAT

/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.8 AVB Interface Descriptor */
/* Milan v1.2, Sec. 5.3.3.5 */

#define DSC_AVB_INTERFACE_LOCALIZED_DESCRIPTION AVB_AEM_DESC_INVALID
#define DSC_AVB_INTERFACE_INTERFACE_FLAGS (AVB_AEM_DESC_AVB_INTERFACE_FLAG_GPTP_GRANDMASTER_SUPPORTED | \
    AVB_AEM_DESC_AVB_INTERFACE_FLAG_GPTP_SUPPORTED | \
    AVB_AEM_DESC_AVB_INTERFACE_FLAG_SRP_SUPPORTED)
// TODO: This is a dynamic parameter
#define DSC_AVB_INTERFACE_CLOCK_IDENTITY 0x3cc0c6FFFE0002CB
#define DSC_AVB_INTERFACE_PRIORITY1 0xF8
#define DSC_AVB_INTERFACE_CLOCK_CLASS 0xF8
#define DSC_AVB_INTERFACE_OFFSET_SCALED_LOG_VARIANCE 0x436A
#define DSC_AVB_INTERFACE_CLOCK_ACCURACY 0x21
#define DSC_AVB_INTERFACE_PRIORITY2 0xF8
#define DSC_AVB_INTERFACE_DOMAIN_NUMBER 0
#define DSC_AVB_INTERFACE_LOG_SYNC_INTERVAL 0
#define DSC_AVB_INTERFACE_LOG_ANNOUNCE_INTERVAL 0
#define DSC_AVB_INTERFACE_PDELAY_INTERVAL 0
#define DSC_AVB_INTERFACE_PORT_NUMBER 0

/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.9 CLOCK_SOURCE Descriptor */
/* Milan v1.2, Sec. 5.3.3.6 */

// Internal Clock Source
#define DSC_CLOCK_SOURCE_INTERNAL_OBJECT_NAME "Internal"
#define DSC_CLOCK_SOURCE_INTERNAL_LOCALIZED_DESCRIPTION AVB_AEM_DESC_INVALID
#define DSC_CLOCK_SOURCE_INTERNAL_FLAGS 0x0002
#define DSC_CLOCK_SOURCE_INTERNAL_TYPE AVB_AEM_DESC_CLOCK_SOURCE_TYPE_INTERNAL
#define DSC_CLOCK_SOURCE_INTERNAL_IDENTIFIER 0
#define DSC_CLOCK_SOURCE_INTERNAL_LOCATION_TYPE AVB_AEM_DESC_CLOCK_SOURCE
#define DSC_CLOCK_SOURCE_INTERNAL_LOCATION_INDEX 0

// AAF Stream Clock Source
#define DSC_CLOCK_SOURCE_AAF_OBJECT_NAME "Stream Clock"
#define DSC_CLOCK_SOURCE_AAF_LOCALIZED_DESCRIPTION AVB_AEM_DESC_INVALID
#define DSC_CLOCK_SOURCE_AAF_FLAGS 0x0002
#define DSC_CLOCK_SOURCE_AAF_TYPE AVB_AEM_DESC_CLOCK_SOURCE_TYPE_INPUT_STREAM
#define DSC_CLOCK_SOURCE_AAF_IDENTIFIER 0
#define DSC_CLOCK_SOURCE_AAF_LOCATION_TYPE AVB_AEM_DESC_STREAM_INPUT
#define DSC_CLOCK_SOURCE_AAF_LOCATION_INDEX 0

// CRF Clock Source
#define DSC_CLOCK_SOURCE_CRF_OBJECT_NAME "CRF Clock"
#define DSC_CLOCK_SOURCE_CRF_LOCALIZED_DESCRIPTION AVB_AEM_DESC_INVALID
#define DSC_CLOCK_SOURCE_CRF_FLAGS 0x0002
#define DSC_CLOCK_SOURCE_CRF_TYPE AVB_AEM_DESC_CLOCK_SOURCE_TYPE_INPUT_STREAM
#define DSC_CLOCK_SOURCE_CRF_IDENTIFIER 0
#define DSC_CLOCK_SOURCE_CRF_LOCATION_TYPE AVB_AEM_DESC_STREAM_INPUT
#define DSC_CLOCK_SOURCE_CRF_LOCATION_INDEX 1

/**************************************************************************************/
/* IEEE 1722.1-2021, Sec. 7.2.32 CLOCK_DOMAIN Descriptor */
/* Milan v1.2, Sec. 5.3.3.11 */

#define DSC_CLOCK_DOMAIN_OBJECT_NAME "Clock Reference Format"
#define DSC_CLOCK_DOMAIN_LOCALIZED_DESCRIPTION AVB_AEM_DESC_INVALID
#define DSC_CLOCK_DOMAIN_CLOCK_SOURCE_INDEX 0
#define DSC_CLOCK_DOMAIN_DESCRIPTOR_COUNTS_OFFSET (4 + sizeof(struct avb_aem_desc_clock_domain))
#define DSC_CLOCK_DOMAIN_CLOCK_SOURCES_COUNT 3

#define DSC_CLOCK_DOMAIN_SOURCES_0 0  // Internal
#define DSC_CLOCK_DOMAIN_SOURCES_1 1  // AAF
#define DSC_CLOCK_DOMAIN_SOURCES_2 2  // CRF

#endif // __DESCRIPTOR_ENTITY_MODEL_MILAN_H__
