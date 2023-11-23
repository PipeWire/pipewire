#ifndef MODULE_ROC_COMMON_H
#define MODULE_ROC_COMMON_H

#include <roc/config.h>
#include <roc/endpoint.h>

#include <spa/utils/string.h>

#define PW_ROC_DEFAULT_IP "0.0.0.0"
#define PW_ROC_DEFAULT_SOURCE_PORT 10001
#define PW_ROC_DEFAULT_REPAIR_PORT 10002
#define PW_ROC_DEFAULT_CONTROL_PORT 10003
#define PW_ROC_DEFAULT_SESS_LATENCY 200
#define PW_ROC_DEFAULT_RATE 44100
#define PW_ROC_DEFAULT_CONTROL_PROTO ROC_PROTO_RTCP

static inline int pw_roc_parse_fec_encoding(roc_fec_encoding *out, const char *str)
{
	if (!str || !*str)
		*out = ROC_FEC_ENCODING_DEFAULT;
	else if (spa_streq(str, "disable"))
		*out = ROC_FEC_ENCODING_DISABLE;
	else if (spa_streq(str, "rs8m"))
		*out = ROC_FEC_ENCODING_RS8M;
	else if (spa_streq(str, "ldpc"))
		*out = ROC_FEC_ENCODING_LDPC_STAIRCASE;
	else
		return -EINVAL;
	return 0;
}

static inline int pw_roc_parse_resampler_profile(roc_resampler_profile *out, const char *str)
{
	if (!str || !*str)
		*out = ROC_RESAMPLER_PROFILE_DEFAULT;
	else if (spa_streq(str, "high"))
		*out = ROC_RESAMPLER_PROFILE_HIGH;
	else if (spa_streq(str, "medium"))
		*out = ROC_RESAMPLER_PROFILE_MEDIUM;
	else if (spa_streq(str, "low"))
		*out = ROC_RESAMPLER_PROFILE_LOW;
	else
		return -EINVAL;
	return 0;
}

static inline int pw_roc_create_endpoint(roc_endpoint **result, roc_protocol protocol, const char *ip, int port)
{
	roc_endpoint *endpoint;

	if (roc_endpoint_allocate(&endpoint))
		return -ENOMEM;

	if (roc_endpoint_set_protocol(endpoint, protocol))
		goto out_error_free_ep;

	if (roc_endpoint_set_host(endpoint, ip))
		goto out_error_free_ep;

	if (roc_endpoint_set_port(endpoint, port))
		goto out_error_free_ep;

	*result = endpoint;
	return 0;

out_error_free_ep:
	(void) roc_endpoint_deallocate(endpoint);
	return -EINVAL;
}

static inline void pw_roc_fec_encoding_to_proto(roc_fec_encoding fec_code, roc_protocol *audio, roc_protocol *repair)
{
	switch (fec_code) {
	case ROC_FEC_ENCODING_DEFAULT:
	case ROC_FEC_ENCODING_RS8M:
		*audio = ROC_PROTO_RTP_RS8M_SOURCE;
		*repair = ROC_PROTO_RS8M_REPAIR;
		break;
	case ROC_FEC_ENCODING_LDPC_STAIRCASE:
		*audio = ROC_PROTO_RTP_LDPC_SOURCE;
		*repair = ROC_PROTO_LDPC_REPAIR;
		break;
	default:
		*audio = ROC_PROTO_RTP;
		*repair = 0;
		break;
	}
}

#endif /* MODULE_ROC_COMMON_H */
