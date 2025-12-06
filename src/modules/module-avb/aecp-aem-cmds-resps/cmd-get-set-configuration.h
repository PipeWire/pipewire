#ifndef __AECP_AEM_CMD_GET_SET_CONFIGURATION_H__
#define __AECP_AEM_CMD_GET_SET_CONFIGURATION_H__

int handle_cmd_set_configuration_milan_v12(struct aecp *aecp, int64_t now,
                                const void *m, int len);

int handle_cmd_get_configuration_common(struct aecp *aecp, int64_t now,
    const void *m, int len);

#endif //__AECP_AEM_CMD_GET_SET_CONFIGURATION_H__
