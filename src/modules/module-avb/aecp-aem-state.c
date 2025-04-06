#include "aecp-aem-state.h"
#include "utils.h"
#include "internal.h"

struct aecp_aem_state_handlers {
    // TODO lock?
    const struct aem_state_var_info *var_info;
    void* (*getter_h) (struct aecp*, uint64_t target_id);
    int (*setter_h) (struct aecp*, uint64_t target_id, void *state);
    struct aecp_aem_state *var_data;
};

#define AECP_AEM_STATE(id, getter, setter) \
		[id] = { .getter_h = getter, .setter_h = setter, .var_data = NULL }

/** forward declaration */

static void* aecp_aem_lock_get(struct aecp* aecp, uint64_t target_id);
static int aecp_aem_lock_set(struct aecp* aecp, uint64_t target_id, void *state);

static struct aecp_aem_state_handlers ae_state_handlers[] = {
    AECP_AEM_STATE(aecp_aem_lock, aecp_aem_lock_get, aecp_aem_lock_set),
};

static void* aecp_aem_lock_get(struct aecp* aecp, uint64_t target_id)
{
    struct aecp_aem_lock_state *lock =
        (struct aecp_aem_lock_state *) ae_state_handlers[aecp_aem_lock].var_data;

    if (!lock) {
        return NULL;
    }

    return lock;
}

static int aecp_aem_lock_set(struct aecp* aecp, uint64_t target_id, void *state)
{
    struct aecp_aem_lock_state *lock =
        (struct aecp_aem_lock_state *) ae_state_handlers[aecp_aem_lock].var_data;

    if (!lock) {
        return -1;
    }

    memcpy(lock, state, sizeof(*lock));

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
        return -1;
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

void* aecp_aem_get_state_var(struct aecp* aecp, uint64_t target_id,
    enum aecp_aem_lock_types type)
{
    void *ae_state;

    pw_log_info("aecm state for %lu type %d getter\n", target_id, type);
    if ((type >= aecp_aem_max) || (type <= aecp_aem_min)) {
        pw_log_error("get: aecp state type %u is not supported\n", type);
        return NULL;
    }

    ae_state = ae_state_handlers[type].getter_h(aecp, target_id);

    return ae_state;
}

int aecp_aem_set_state_var(struct aecp* aecp, uint64_t target_id,
    enum aecp_aem_lock_types type, void *state)
{
    int rc;

    pw_log_info("aecm state for %lu type %d getter\n", target_id, type);
    if ((type >= aecp_aem_max) || (type <= aecp_aem_min)) {
        pw_log_error("set: aecp state type %u is not supported\n", type);
        return -1;
    }

    rc = ae_state_handlers[type].setter_h(aecp, target_id, state);
    if (rc) {
        return rc;
    }

    return 0;
}

int aecp_aem_init_var_containers(struct aecp *aecp,
                const struct aem_state_var_info *varsdesc, size_t array_size)
{
    const char *var_name;
    size_t el_sz;
    size_t count;

    uint64_t target_id = aecp->server->entity_id;
    if ((size_t)ARRAY_SIZE(ae_state_handlers) < array_size) {
        pw_log_error("could not init the container, error in the var init");
        return -1;
    }

    for (size_t vars = 0; vars < array_size; vars++) {
        var_name = varsdesc[vars].var_name;

        if (var_name == NULL) {
            // Part of the empty table
            continue;
        }

        el_sz = varsdesc[vars].el_sz;
        count = varsdesc[vars].count;
        pw_log_info("adding var %s to %lu %ld element of size %ld\n",
            var_name, target_id, count, el_sz);

        if (ae_state_handlers[vars].var_info != NULL) {
            pw_log_error("%s type %ld already set\n",
                            varsdesc[vars].var_name, vars);
            return -1;
        }

        ae_state_handlers[vars].var_info = &varsdesc[vars];
        if (!aecp_aem_create(aecp, target_id, vars, &varsdesc[vars])) {
            pw_log_error("%s type %ld not created\n",
                varsdesc[vars].var_name, vars);
        }
    }

    return 0;
}