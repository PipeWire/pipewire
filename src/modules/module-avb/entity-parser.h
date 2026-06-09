#ifndef ENTITY_PARSER_H
#define ENTITY_PARSER_H

#include "aecp-aem-descriptors.h"
#include "avb.h"
#include "entity-model-milan-v12.h"
#include "internal.h"
#include "descriptors.h"
#include "strings.h"

struct avb_entity_config {
    char entity_name[64];
    char group_name[64];
    char serial_number[64];
    char firmware_version[64];
    uint16_t vendor_name;
    uint16_t model_name;
    uint16_t talker_capabilities;
    uint16_t listener_capabilities;
    uint32_t entity_capabilities;
    uint32_t controller_capabilities;
};

/* Default entity configuration, taken from the compiled-in Milan entity model. */
#define AVB_ENTITY_CONFIG_DEFAULT ((struct avb_entity_config) {			\
    .entity_name = DSC_ENTITY_MODEL_ENTITY_NAME,                        \
    .group_name = DSC_ENTITY_MODEL_GROUP_NAME,                          \
    .serial_number = DSC_ENTITY_MODEL_SERIAL_NUMBER,                    \
    .firmware_version = DSC_ENTITY_MODEL_FIRMWARE_VERSION,              \
    .vendor_name = DSC_ENTITY_MODEL_VENDOR_NAME_STRING,                 \
    .model_name = DSC_ENTITY_MODEL_MODEL_NAME_STRING,                   \
    .talker_capabilities = DSC_ENTITY_MODEL_TALKER_CAPABILITIES,        \
    .listener_capabilities = DSC_ENTITY_MODEL_LISTENER_CAPABILITIES,    \
    .entity_capabilities = DSC_ENTITY_MODEL_ENTITY_CAPABILITIES,        \
    .controller_capabilities = DSC_ENTITY_MODEL_CONTROLLER_CAPABILITIES,\
})

/* Override a 64-byte, zero-padded AVB string field from a property. The default
 * already in dst is kept when the key is missing or the value is not valid
 * UTF-8 (IEEE 1722.1, Sec. 7.4.17.1). */
static inline void conf_load_string(char dst[64], const struct pw_properties *props,
        const char *key)
{
    size_t len;
    const char *val = pw_properties_get(props, key);
    if (val == NULL)
        return;

    len = strnlen(val, 64);
    if (validate_utf8((const unsigned char *)val, len) != 0) {
        pw_log_warn("entity.%s is not valid UTF-8, keeping default", key);
        return;
    }

    memcpy(dst, val, len);
    memset(dst + len, 0, 64 - len);
}

/* Override a uint16_t field when the key is present and parses as an integer. */
static inline void conf_load_u16(uint16_t *dst, const struct pw_properties *props,
        const char *key)
{
    uint32_t val;
    if (pw_properties_fetch_uint32(props, key, &val) == 0) {
        *dst = (uint16_t)val;
    }
}

/* Override a uint32_t field when the key is present and parses as an integer. */
static inline void conf_load_u32(uint32_t *dst, const struct pw_properties *props,
        const char *key)
{
    uint32_t val;
    if (pw_properties_fetch_uint32(props, key, &val) == 0) {
        *dst = val;
    }
}

/* Fill entity_conf from the "entity" dict in avb.properties, starting from the
 * compiled-in defaults. Any field absent or invalid keeps its default value. */
static inline void conf_load_entity(struct pw_properties *props,
        struct avb_entity_config *entity_conf)
{
    struct pw_properties *entity_props;
    /* Start from the defaults; everything below only overrides. */
    *entity_conf = AVB_ENTITY_CONFIG_DEFAULT;

    pw_log_info("Acquiring entity properties from entity dict in avb.properties");
    const char *str = pw_properties_get(props, "entity");
    if (str == NULL)
        return;

    /* The "entity" property is itself a serialized dict; parse it so the
     * individual entity fields can be read back by key. */
    entity_props = pw_properties_new(NULL, NULL);
    if (entity_props == NULL)
        return;
    pw_properties_update_string(entity_props, str, strlen(str));

    pw_log_info("Assigning entity properties");
    conf_load_string(entity_conf->entity_name, entity_props, "entity_name");
    conf_load_string(entity_conf->group_name, entity_props, "group_name");
    conf_load_string(entity_conf->serial_number, entity_props, "serial_number");
    conf_load_string(entity_conf->firmware_version, entity_props, "firmware_version");

    conf_load_u16(&entity_conf->vendor_name, entity_props, "vendor_name");
    conf_load_u16(&entity_conf->model_name, entity_props, "model_name");
    conf_load_u16(&entity_conf->talker_capabilities, entity_props, "talker_capabilities");
    conf_load_u16(&entity_conf->listener_capabilities, entity_props, "listener_capabilities");
    conf_load_u32(&entity_conf->entity_capabilities, entity_props, "entity_capabilities");
    conf_load_u32(&entity_conf->controller_capabilities, entity_props, "controller_capabilities");

    pw_properties_free(entity_props);
}

#endif /* ENTITY_PARSER_H */
