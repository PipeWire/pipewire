#ifndef __AECP_AEM_NAME_COMMON_H__
#define __AECP_AEM_NAME_COMMON_H__

#include <stdint.h>
#include "../aecp-aem-descriptors.h"

#define AECP_AEM_NAME_INDEX_ENTITY_ITSELF       (0)
#define AECP_AEM_NAME_INDEX_ENTITY_GROUP        (1)

static const bool list_support_descriptors_setget_name[AVB_AEM_DESC_MAX_17221] = {
    [AVB_AEM_DESC_ENTITY] = true,
    [AVB_AEM_DESC_CONFIGURATION] = true,
    [AVB_AEM_DESC_AUDIO_UNIT] = true,
    [AVB_AEM_DESC_VIDEO_UNIT] = true,
    [AVB_AEM_DESC_STREAM_INPUT] = true,
    [AVB_AEM_DESC_STREAM_OUTPUT] = true,
    [AVB_AEM_DESC_JACK_INPUT] = true,
    [AVB_AEM_DESC_JACK_OUTPUT] = true,
    [AVB_AEM_DESC_AVB_INTERFACE] = true,
    [AVB_AEM_DESC_CLOCK_SOURCE] = true,
    [AVB_AEM_DESC_MEMORY_OBJECT] = true,
    [AVB_AEM_DESC_AUDIO_CLUSTER] = true,
    [AVB_AEM_DESC_VIDEO_CLUSTER] = true,
    [AVB_AEM_DESC_SENSOR_CLUSTER] = true,
    [AVB_AEM_DESC_CONTROL] = true,
    [AVB_AEM_DESC_SIGNAL_SELECTOR] = true,
    [AVB_AEM_DESC_MIXER] = true,
    [AVB_AEM_DESC_MATRIX] = true,
    [AVB_AEM_DESC_SIGNAL_SPLITTER] = true,
    [AVB_AEM_DESC_SIGNAL_COMBINER] = true,
    [AVB_AEM_DESC_SIGNAL_DEMULTIPLEXER] = true,
    [AVB_AEM_DESC_SIGNAL_MULTIPLEXER] = true,
    [AVB_AEM_DESC_SIGNAL_TRANSCODER] = true,
    [AVB_AEM_DESC_CLOCK_DOMAIN] = true,
    [AVB_AEM_DESC_CONTROL_BLOCK] = true,
    [AVB_AEM_DESC_TIMING] = true,
    [AVB_AEM_DESC_PTP_INSTANCE] = true,
    [AVB_AEM_DESC_PTP_PORT] = true,
};

static inline int aem_aecp_get_name_entity(struct descriptor *desc, uint16_t str_idex,
    char **desc_name)
{
   struct avb_aem_desc_entity *entity =
           (struct avb_aem_desc_entity* ) desc->ptr;

   char *dest;
   if (str_idex == AECP_AEM_NAME_INDEX_ENTITY_ITSELF) {
       dest = entity->entity_name;
   } else if (str_idex == AECP_AEM_NAME_INDEX_ENTITY_GROUP) {
       dest = entity->group_name;
   } else {
        pw_log_error("Could not get the name of the entity for \n");
        pw_log_error("type %d, idx %d, str_idx %d\n", desc->type, desc->index,
                    str_idex);
       spa_assert(0);
   }

   *desc_name = dest;
   return 0;
}

static inline int reply_set_name(struct aecp *aecp, const void *m, int len, int status, const char *name)
{
    uint8_t buf[512];
    struct avb_ethernet_header *h = (void*)buf;
    struct avb_packet_aecp_header *reply = SPA_PTROFF(h, sizeof(*h), void);
    struct avb_packet_aecp_aem *p_reply = (void*)reply;
    struct avb_packet_aecp_aem_setget_name *ae_reply;

    memcpy(buf, m, len);
    // Point to payload of AEM command
    ae_reply = (struct avb_packet_aecp_aem_setget_name *)p_reply->payload;

    // Set message type to response and a valid status
    AVB_PACKET_AECP_SET_MESSAGE_TYPE(reply, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
    AVB_PACKET_AECP_SET_STATUS(reply, status);

    // Include the name in the response
	memcpy(ae_reply->name, name, AECP_AEM_STRLEN_MAX);

    return avb_server_send_packet(aecp->server, h->src, AVB_TSN_ETH, buf, len);
}

#endif //__AECP_AEM_NAME_COMMON_H__