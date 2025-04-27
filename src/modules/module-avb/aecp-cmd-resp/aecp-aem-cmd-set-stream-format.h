#ifndef __AECP_AEM_CMD_SET_FORMAT_H__
#define __AECP_AEM_CMD_SET_FORMAT_H__

#include "aecp-aem-cmd-resp-common.h"

int handle_cmd_set_stream_format(struct aecp *aecp, int64_t now, const void *m, int len);
int handle_unsol_set_stream_format(struct aecp *aecp, int64_t now);


#endif // __AECP_AEM_CMD_SET_FORMAT_H__