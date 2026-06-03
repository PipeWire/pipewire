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

static inline struct avb_entity_config conf_load_entity (struct pw_properties *props) {
	// Grab entity field and turn into pw_properties struct
	struct avb_entity_config entity_conf;
	const char *str;
	pw_log_info("Acquiring entity properties from entity dict in avb.properties");
        str = pw_properties_get(props, "entity");
        struct pw_properties *entity_props;
	entity_props = pw_properties_new(NULL,NULL);
        pw_properties_update_string(entity_props, str, strlen(str));

	// Assign properties to aem_entity_config struct
	// First handle strings
	//TODO: with strings, check utf8 format and set as a filled 64 byte char array
	// check zero padding and utf8 format
	pw_log_info("Assigning entity properties");
        const char *name = pw_properties_get(entity_props, "entity_name");
	const char *entity_name;

	int use_default = name ? 1 : 0;
	size_t len = 0;

	if (name) {
		len = strnlen(name, 64);
		if (validate_utf8((uint8_t*)name, len))
			entity_name = name;
		else
			use_default = 1;
	}
	if (use_default) {
		len = strnlen(name, 64);
		entity_name = (char *)DSC_ENTITY_MODEL_ENTITY_NAME;
	}
	memcpy(entity_conf.entity_name, entity_name, 64);
	memset(entity_conf.entity_name + len, 0, 64 - len);

	const char *serial = pw_properties_get(entity_props, "serial_number");
	const char *serial_number = serial ? serial : DSC_ENTITY_MODEL_SERIAL_NUMBER;
	strncpy(entity_conf.serial_number, serial_number, sizeof(entity_conf.serial_number));

	const char *firmware = pw_properties_get(entity_props, "firmware_version");
	const char *firmware_version = firmware ? firmware : DSC_ENTITY_MODEL_FIRMWARE_VERSION;
	strncpy(entity_conf.firmware_version, firmware_version, sizeof(entity_conf.firmware_version));

	const char *group = pw_properties_get(entity_props, "group_name");
	const char *group_name = group ? group: DSC_ENTITY_MODEL_GROUP_NAME;
	strncpy(entity_conf.group_name, group_name, sizeof(entity_conf.group_name));

	// Handle integers
	uint32_t vendor_name;
	int vn_found = pw_properties_fetch_uint32(entity_props, "vendor_name", &vendor_name);
	entity_conf.vendor_name = !vn_found ? (uint16_t)vendor_name : DSC_ENTITY_MODEL_VENDOR_NAME_STRING;

	uint32_t model_name;
	int mn_found = pw_properties_fetch_uint32(entity_props, "model_name", &model_name);
	entity_conf.model_name = !mn_found ? (uint16_t)model_name : DSC_ENTITY_MODEL_MODEL_NAME_STRING;

	uint32_t talker_capabilities;
	int tc_found = pw_properties_fetch_uint32(entity_props, "talker_capabilities", &talker_capabilities);
	entity_conf.talker_capabilities  = !tc_found ? (uint16_t)talker_capabilities : DSC_ENTITY_MODEL_TALKER_CAPABILITIES;

	uint32_t listener_capabilities;
	int lc_found =  pw_properties_fetch_uint32(entity_props, "listener_capabilities", &listener_capabilities);
	entity_conf.listener_capabilities = !lc_found ? (uint16_t)listener_capabilities : DSC_ENTITY_MODEL_LISTENER_CAPABILITIES;

	uint32_t entity_capabilities;
	int ec_found = pw_properties_fetch_uint32(entity_props, "entity_capabilities", &entity_capabilities);
	entity_conf.entity_capabilities = !ec_found ? entity_capabilities : DSC_ENTITY_MODEL_ENTITY_CAPABILITIES;

	uint32_t controller_capabilities;
	int cc_found = pw_properties_fetch_uint32(entity_props, "controller_capabilities", &controller_capabilities);
	entity_conf.controller_capabilities = !cc_found ? controller_capabilities : DSC_ENTITY_MODEL_CONTROLLER_CAPABILITIES;

        return entity_conf;
}

#endif /* ENTITY_PARSER_H */
