/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include "aecp-aem-state.h"
#include "aecp-aem.h"
#include "utils.h"
#include "internal.h"

struct aecp_aem_state_handlers {
    // TODO lock?
    int (*getter_h) (struct aecp*, const struct aecp_aem_state_handlers*,
        enum aecp_aem_lock_types, uint64_t, uint16_t, void *);

    int (*setter_h) (struct aecp*, const struct aecp_aem_state_handlers*,
        enum aecp_aem_lock_types, uint64_t, uint16_t, void *);

    bool (*update_chk_h) (struct aecp*, uint64_t target_id, uint16_t id);
    void *var_data;
};

#define AECP_AEM_STATE(id, getter, setter, update_chk) \
		[id] = { .getter_h = getter, .setter_h = setter,\
                    .update_chk_h = update_chk, .var_data = NULL }

/** forward declaration */
static int aecp_aem_generic_get(struct aecp* aecp,
    const struct aecp_aem_state_handlers* info, enum aecp_aem_lock_types type,
    uint64_t target_id, uint16_t id, void *state);

static int aecp_aem_generic_set(struct aecp* aecp,
    const struct aecp_aem_state_handlers* info, enum aecp_aem_lock_types type,
    uint64_t target_id, uint16_t id, void *state);

/** Table listing of the set / get for a specific var */
static struct aecp_aem_state_handlers ae_state_handlers[] = {
    AECP_AEM_STATE(aecp_aem_lock, aecp_aem_generic_get, aecp_aem_generic_set,
        NULL),

    AECP_AEM_STATE(aecp_aem_name, aecp_aem_generic_get, aecp_aem_generic_set,
        NULL),
 
    AECP_AEM_STATE(aecp_aem_configuration, aecp_aem_generic_get,
        aecp_aem_generic_set, NULL),

    AECP_AEM_STATE(aecp_aem_unsol_notif, aecp_aem_generic_get,
        aecp_aem_generic_set, NULL),
};

/** The definitions of the previously created callbacks */
static int aecp_aem_generic_get(struct aecp* aecp,
    const struct aecp_aem_state_handlers* info, enum aecp_aem_lock_types type,
    uint64_t target_id, uint16_t id, void *state)
{
    uint8_t *var = info->var_data;
    size_t size;
    struct aem_state_var_info* inf =
        (struct aem_state_var_info*) info->var_data;
    size = inf->el_sz;

    if (id >= inf->count) {
        pw_log_error("Invalid %s id=%d\n", inf->var_name, id);
        spa_assert(0);
    }
    if (!var) {
        pw_log_error("Null var %s id=%d\n", inf->var_name, id);
        spa_assert(0);
    }

    memcpy(state, &var[id*size], size);

    return 0;
}

static int aecp_aem_generic_set(struct aecp* aecp,
    const struct aecp_aem_state_handlers* info, enum aecp_aem_lock_types type,
    uint64_t target_id, uint16_t id, void *state)
{
    uint8_t *var = info->var_data;
    size_t size;
    struct aem_state_var_info* inf =
        (struct aem_state_var_info*) info->var_data;
    size = inf->el_sz;

    if (id >= inf->count) {
        pw_log_error("Invalid %s id=%d\n", inf->var_name, id);
        spa_assert(0);
    }

    if (!var) {
        pw_log_error("NULL var %s id=%d\n", inf->var_name, id);
        spa_assert(0);
    }

    memcpy(&var[id*size], state, size);
    return 0;
}

void* aecp_aem_create(struct aecp* aecp, uint64_t target_id,
    enum aecp_aem_lock_types type, const struct aem_state_var_info* var)
{
    void *ptr_created;

    if ((type >= aecp_aem_max) || (type <= aecp_aem_min)) {
        pw_log_error("aecp state type %u is not supported\n", type);
        return NULL;
    }

    if (NULL != avb_aecp_aem_find_state_var(aecp->server, target_id, type)) {
        pw_log_warn("Could not add aecp state type %u for target %lu\n",
                    type, target_id);

        return NULL;
    }

    /** This is used to keep track of the vars created */
    ptr_created = avb_aecp_aem_add_state_var(aecp->server, target_id, type,
        var->el_sz * var->count);

    if (!ptr_created) {
        pw_log_error("aem create: failed %u, %lu", type, target_id);
        return NULL;
    }

    /* TODO use a list per var when multiple entity hits */
    ae_state_handlers[type].var_data = ptr_created;

    return ptr_created;
}

int aecp_aem_delete(struct aecp* aecp, uint64_t target_id,
    enum aecp_aem_lock_types type, size_t size)
{
    void *ptr_found;

    if ((type >= aecp_aem_max) || (type <= aecp_aem_min)) {
        pw_log_error("create: aecp state type %u is not supported\n", type);
        spa_assert(0);
    }

    ptr_found = avb_aecp_aem_find_state_var(aecp->server, target_id, type);
    if (NULL == ptr_found) {
        pw_log_warn("Could not find aecp state type %u for target %lu\n",
                    type, target_id);

        return -1;
    }

    avb_aecp_aem_remove(aecp->server, ptr_found);
    return 0;
}

int aecp_aem_get_state_var(struct aecp* aecp, uint64_t target_id,
    enum aecp_aem_lock_types type, uint16_t id, void *state)
{
    int rc;

    pw_log_info("get: aecm state for %lx type %d getter\n", target_id, type);
    if ((type >= aecp_aem_max) || (type <= aecp_aem_min)) {
        pw_log_error("get: aecp state type %u is not supported\n", type);
        spa_assert(0);
    }

    struct aem_state_var_info* inf =
        (struct aem_state_var_info*) ae_state_handlers[type].var_data;

    if (!ae_state_handlers[type].getter_h) {
        pw_log_error("Missing getter for var[%d] %s\n", type, inf->var_name);
        spa_assert(0);
    }

    rc = ae_state_handlers[type].getter_h(aecp, &ae_state_handlers[type],
            type, target_id, id, state);

    return rc;
}

int aecp_aem_refresh_state_var(struct aecp* aecp, uint64_t target_id,
    enum aecp_aem_lock_types type, uint16_t id, void *state)
{
    int rc;
    if ((type >= aecp_aem_max) || (type <= aecp_aem_min)) {
        pw_log_error("set: aecp state type %u is not supported\n", type);
        spa_assert(0);
    }

    struct aem_state_var_info* inf =
        (struct aem_state_var_info*) ae_state_handlers[type].var_data;

    if (!ae_state_handlers[type].setter_h) {
        pw_log_error("Missing setter for var[%d] %s\n", type, inf->var_name);
        spa_assert(0);
    }

    rc = ae_state_handlers[type].setter_h(aecp, &ae_state_handlers[type],
        type, target_id, id, state);
    if (rc) {
        return -1;
    }

    return 0;
}

int aecp_aem_get_base_info(struct aecp* aecp, uint64_t target_id,
    enum aecp_aem_lock_types type, uint16_t id, struct aecp_aem_base_info **info)
{
    uint8_t *data;
    struct aem_state_var_info *vinfo;
    int rc = 0;
    data = ae_state_handlers[type].var_data;

    if (data == NULL) {
        rc = -1;
    }

    data = ae_state_handlers[type].var_data;
    vinfo = (struct aem_state_var_info*) data;
    *info = (struct aecp_aem_base_info*) &data[id*vinfo->el_sz];

    return rc;
}

int aecp_aem_set_state_var(struct aecp* aecp, uint64_t target_id,
    uint64_t ctrler_id, enum aecp_aem_lock_types type, uint16_t id, void *state)
{
    uint8_t *data;
    int rc;
    struct aecp_aem_base_info *info;

    rc = aecp_aem_refresh_state_var(aecp, target_id, type, id, state);
    if (rc) {
        return -1;
    }

    // Always expected one must be available */
    info = (struct aecp_aem_base_info *) ae_state_handlers[type].var_data;
    data = ae_state_handlers[type].var_data;

    info = (struct aecp_aem_base_info *) &data[id*info->var_info.el_sz];
    info->controller_entity_id = ctrler_id;
    info->needs_update = true;

    return 0;
}

int aecp_aem_init_var_containers(struct aecp *aecp,
                const struct aem_state_var_info *varsdesc, size_t array_size)
{
    const char *var_name;
    size_t el_sz;
    size_t count;
    size_t vars;
    pw_log_info("Initializing variables\n");
    uint64_t target_id = aecp->server->entity_id;
    if ((size_t)ARRAY_SIZE(ae_state_handlers) != array_size) {
        pw_log_error("could not init the container, error in the var init");
        pw_log_error("The count of var handling is different tha the array_size");
        spa_assert(0);
    }

    for (vars = 0; vars < array_size; vars++) {
        var_name = varsdesc[vars].var_name;

        if (var_name == NULL) {
            // Part of the empty table
            continue;
        }

        el_sz = varsdesc[vars].el_sz;
        count = varsdesc[vars].count;

        if (!aecp_aem_create(aecp, target_id, vars, &varsdesc[vars])) {
            pw_log_error("%s type %ld not created\n",
                varsdesc[vars].var_name, vars);
            spa_assert(0);
        }

        pw_log_info("adding var %s to %lx %ld element of size %ld\n",
            var_name, target_id, count, el_sz);

        uint8_t *data = ae_state_handlers[vars].var_data;
        for (size_t idx = 0; idx < varsdesc[vars].count; idx++) {
            pw_log_info("%s type %ld created\n",
                        varsdesc[vars].var_name, vars);
            // Here copy the data so we can have it in as an inhereited value
            memcpy(&data[idx * varsdesc[vars].el_sz],
                &varsdesc[vars], sizeof(struct aem_state_var_info));
        }

    }

    pw_log_info("Done creating %ld vars\n", vars);


    return 0;
}