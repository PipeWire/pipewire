/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif
#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif

#include <spa/utils/cleanup.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>

#include <pipewire/impl.h>
#include <pipewire/private.h>

/** \page page_module_scheduler_v1 SchedulerV1
 *
 *
 * ## Module Name
 *
 * `libpipewire-module-scheduler-v1`
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * ## Config override
 *
 * A `module.scheduler-v1.args` config section can be added
 * to override the module arguments.
 *
 *\code{.unparsed}
 * # ~/.config/pipewire/pipewire.conf.d/my-scheduler-v1-args.conf
 *
 * module.scheduler-v1.args = {
 * }
 *\endcode
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * context.modules = [
 *  {   name = libpipewire-module-scheduler-v1
 *      args = {
 *      }
 *  }
 *]
 *\endcode
 *
 * Since: 1.7.0
 */

#define NAME "scheduler-v1"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE	""

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@proton.me>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Implement the Scheduler V1" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#define MAX_HOPS	64
#define MAX_SYNC	4u

struct impl {
	struct pw_context *context;

	struct pw_properties *props;

	struct spa_hook context_listener;
	struct spa_hook module_listener;
};

static int ensure_state(struct pw_impl_node *node, bool running)
{
	enum pw_node_state state = node->info.state;
	if (node->active && node->runnable &&
	    !SPA_FLAG_IS_SET(node->spa_flags, SPA_NODE_FLAG_NEED_CONFIGURE) && running)
		state = PW_NODE_STATE_RUNNING;
	else if (state > PW_NODE_STATE_IDLE)
		state = PW_NODE_STATE_IDLE;
	return pw_impl_node_set_state(node, state);
}

/* make a node runnable. This will automatically also make all non-passive peer nodes
 * runnable and the nodes that belong to the same groups or link_groups. We stop when
 * we reach a passive port.
 *
 * We have 4 cases for the links:
 * (p) marks a passive port. we don't follow the peer from this port.
 *
 *  A   ->   B   ==> B can also be runnable
 *  A  p->   B   ==> B can also be runnable
 *  A   ->p  B   ==> B can not be runnable
 *  A  p->p  B   ==> B can not be runnable
 */
static void make_runnable(struct pw_context *context, struct pw_impl_node *node)
{
	struct pw_impl_port *p;
	struct pw_impl_link *l;
	struct pw_impl_node *n;
	uint32_t n_sync = 0;
	char *sync[MAX_SYNC+1] = { NULL };

	if (!node->runnable) {
		pw_log_debug("%s is runnable", node->name);
		node->runnable = true;
	}

	if (node->sync) {
		for (uint32_t i = 0; node->sync_groups[i]; i++) {
			if (n_sync >= MAX_SYNC)
				break;
			if (pw_strv_find(sync, node->sync_groups[i]) >= 0)
				continue;
			sync[n_sync++] = node->sync_groups[i];
			sync[n_sync] = NULL;
		}
	}
	spa_list_for_each(p, &node->output_ports, link) {
		spa_list_for_each(l, &p->links, output_link) {
			n = l->input->node;
			if (!l->prepared || !n->active || l->input->passive)
				continue;
			if (!n->runnable)
				make_runnable(context, n);
		}
	}
	spa_list_for_each(p, &node->input_ports, link) {
		spa_list_for_each(l, &p->links, input_link) {
			n = l->output->node;
			if (!l->prepared || !n->active || l->output->passive)
				continue;
			if (!n->runnable)
				make_runnable(context, n);
		}
	}
	/* now go through all the nodes that share groups and link_groups
	 * that are not yet runnable. We don't include sync-groups because they
	 * are only used to group the node with a driver, not to determine the
	 * runnable state of a node. */
	if (node->groups != NULL || node->link_groups != NULL || sync[0] != NULL) {
		spa_list_for_each(n, &context->node_list, link) {
			if (n->exported || !n->active || n->runnable)
				continue;
			/* the other node will be scheduled with this one if it's in
			 * the same group or link group */
			if (pw_strv_find_common(n->groups, node->groups) < 0 &&
			    pw_strv_find_common(n->link_groups, node->link_groups) < 0)
				continue;

			make_runnable(context, n);
		}
	}
}

/* check if a node and its peer can run. They can both run if there is a non-passive
 * link between them. The passive link is between 1 or more passive ports.
 *
 * There are 4 cases:
 *
 * (p) marks a passive port. we don't follow the peer from this port.
 * A can not be a driver
 *
 *  A   ->   B   ==> both nodes can run
 *  A   ->p  B   ==> both nodes can run (B is passive so it can't activate A, but
 *                   A can activate B)
 *  A  p->   B   ==> nodes don't run, port A is passive and doesn't activate B
 *  A  p->p  B   ==> nodes don't run
 *
 *  Once we decide the two nodes should be made runnable we do make_runnable()
 *  on both.
 */
static void check_runnable(struct pw_context *context, struct pw_impl_node *node)
{
	struct pw_impl_port *p;
	struct pw_impl_link *l;
	struct pw_impl_node *n;

	if (node->always_process && !node->runnable)
		make_runnable(context, node);

	spa_list_for_each(p, &node->output_ports, link) {
		spa_list_for_each(l, &p->links, output_link) {
			n = l->input->node;
			/* the peer needs to be active and we are linked to it
			 * with a non-passive link */
			if (!n->active || p->passive)
				continue;
			/* explicitly prepare the link in case it was suspended */
			pw_impl_link_prepare(l);
			if (!l->prepared)
				continue;
			make_runnable(context, node);
			make_runnable(context, n);
		}
	}
	spa_list_for_each(p, &node->input_ports, link) {
		spa_list_for_each(l, &p->links, input_link) {
			n = l->output->node;
			if (!n->active || p->passive)
				continue;
			pw_impl_link_prepare(l);
			if (!l->prepared)
				continue;
			make_runnable(context, node);
			make_runnable(context, n);
		}
	}
}

/* Follow all links and groups from node.
 *
 * After this is done, we end up with a list of nodes in collect that are all
 * linked to node.
 *
 * We don't need to care about active nodes or links, we just follow and group everything.
 * The inactive nodes or links will simply not be runnable but will already be grouped
 * correctly when they do become active and prepared.
 */
static int collect_nodes(struct pw_context *context, struct pw_impl_node *node, struct spa_list *collect)
{
	struct spa_list queue;
	struct pw_impl_node *n, *t;
	struct pw_impl_port *p;
	struct pw_impl_link *l;
	uint32_t n_sync;
	char *sync[MAX_SYNC+1];

	pw_log_debug("node %p: '%s'", node, node->name);

	/* start with node in the queue */
	spa_list_init(&queue);
	spa_list_append(&queue, &node->sort_link);
	node->visited = true;

	n_sync = 0;
	sync[0] = NULL;

	/* now follow all the links from the nodes in the queue
	 * and add the peers to the queue. */
	spa_list_consume(n, &queue, sort_link) {
		spa_list_remove(&n->sort_link);
		spa_list_append(collect, &n->sort_link);

		pw_log_debug(" next node %p: '%s' runnable:%u active:%d",
				n, n->name, n->runnable, n->active);

		if (n->sync) {
			for (uint32_t i = 0; n->sync_groups[i]; i++) {
				if (n_sync >= MAX_SYNC)
					break;
				if (pw_strv_find(sync, n->sync_groups[i]) >= 0)
					continue;
				sync[n_sync++] = n->sync_groups[i];
				sync[n_sync] = NULL;
			}
		}

		spa_list_for_each(p, &n->input_ports, link) {
			spa_list_for_each(l, &p->links, input_link) {
				t = l->output->node;
				if (!t->visited) {
					t->visited = true;
					spa_list_append(&queue, &t->sort_link);
				}
			}
		}
		spa_list_for_each(p, &n->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				t = l->input->node;
				if (!t->visited) {
					t->visited = true;
					spa_list_append(&queue, &t->sort_link);
				}
			}
		}
		/* now go through all the nodes that have the same groups and
		 * that are not yet visited */
		if (n->groups != NULL || n->link_groups != NULL || sync[0] != NULL) {
			spa_list_for_each(t, &context->node_list, link) {
				if (t->exported || t->visited)
					continue;
				/* the other node will be scheduled with this one if it's in
				 * the same group, link group or sync group */
				if (pw_strv_find_common(t->groups, n->groups) < 0 &&
				    pw_strv_find_common(t->link_groups, n->link_groups) < 0 &&
				    pw_strv_find_common(t->sync_groups, sync) < 0)
					continue;

				pw_log_debug("%p: %s join group of %s",
						t, t->name, n->name);
				t->visited = true;
				spa_list_append(&queue, &t->sort_link);
			}
		}
		pw_log_debug(" next node %p: '%s' runnable:%u %p %p %p", n, n->name, n->runnable,
				n->groups, n->link_groups, sync);
	}
	return 0;
}

static void move_to_driver(struct pw_context *context, struct spa_list *nodes,
		struct pw_impl_node *driver)
{
	struct pw_impl_node *n;
	pw_log_debug("driver: %p %s runnable:%u", driver, driver->name, driver->runnable);
	spa_list_consume(n, nodes, sort_link) {
		spa_list_remove(&n->sort_link);

		driver->runnable |= n->runnable;

		pw_log_debug(" follower: %p %s runnable:%u driver-runnable:%u", n, n->name,
				n->runnable, driver->runnable);
		pw_impl_node_set_driver(n, driver);
	}
}
static void remove_from_driver(struct pw_context *context, struct spa_list *nodes)
{
	struct pw_impl_node *n;
	spa_list_consume(n, nodes, sort_link) {
		spa_list_remove(&n->sort_link);
		pw_impl_node_set_driver(n, NULL);
		ensure_state(n, false);
	}
}

static inline void get_quantums(struct pw_context *context, uint32_t *def,
		uint32_t *min, uint32_t *max, uint32_t *rate, uint32_t *floor, uint32_t *ceil)
{
	struct settings *s = &context->settings;
	if (s->clock_force_quantum != 0) {
		*def = *min = *max = s->clock_force_quantum;
		*rate = 0;
	} else {
		*def = s->clock_quantum;
		*min = s->clock_min_quantum;
		*max = s->clock_max_quantum;
		*rate = s->clock_rate;
	}
	*floor = s->clock_quantum_floor;
	*ceil = s->clock_quantum_limit;
}

static inline const uint32_t *get_rates(struct pw_context *context, uint32_t *def, uint32_t *n_rates,
		bool *force)
{
	struct settings *s = &context->settings;
	if (s->clock_force_rate != 0) {
		*force = true;
		*n_rates = 1;
		*def = s->clock_force_rate;
		return &s->clock_force_rate;
	} else {
		*force = false;
		*n_rates = s->n_clock_rates;
		*def = s->clock_rate;
		return s->clock_rates;
	}
}
static void reconfigure_driver(struct pw_context *context, struct pw_impl_node *n)
{
	struct pw_impl_node *s;

	spa_list_for_each(s, &n->follower_list, follower_link) {
		if (s == n)
			continue;
		pw_log_debug("%p: follower %p: '%s' suspend",
				context, s, s->name);
		pw_impl_node_set_state(s, PW_NODE_STATE_SUSPENDED);
	}
	pw_log_debug("%p: driver %p: '%s' suspend",
			context, n, n->name);

	if (n->info.state >= PW_NODE_STATE_IDLE)
		n->need_resume = !n->pause_on_idle;
	pw_impl_node_set_state(n, PW_NODE_STATE_SUSPENDED);
}

/* find smaller power of 2 */
static uint32_t flp2(uint32_t x)
{
	x = x | (x >> 1);
	x = x | (x >> 2);
	x = x | (x >> 4);
	x = x | (x >> 8);
	x = x | (x >> 16);
	return x - (x >> 1);
}

/* cmp fractions, avoiding overflows */
static int fraction_compare(const struct spa_fraction *a, const struct spa_fraction *b)
{
	uint64_t fa = (uint64_t)a->num * (uint64_t)b->denom;
	uint64_t fb = (uint64_t)b->num * (uint64_t)a->denom;
	return fa < fb ? -1 : (fa > fb ? 1 : 0);
}

static inline uint32_t calc_gcd(uint32_t a, uint32_t b)
{
	while (b != 0) {
		uint32_t temp = a;
		a = b;
		b = temp % b;
	}
	return a;
}

struct rate_info {
	uint32_t rate;
	uint32_t gcd;
	uint32_t diff;
};

static inline void update_highest_rate(struct rate_info *best, struct rate_info *current)
{
	/* find highest rate */
	if (best->rate == 0 || best->rate < current->rate)
		*best = *current;
}

static inline void update_nearest_gcd(struct rate_info *best, struct rate_info *current)
{
	/* find nearest GCD */
	if (best->rate == 0 ||
	    (best->gcd < current->gcd) ||
	    (best->gcd == current->gcd && best->diff > current->diff))
		*best = *current;
}
static inline void update_nearest_rate(struct rate_info *best, struct rate_info *current)
{
	/* find nearest rate */
	if (best->rate == 0 || best->diff > current->diff)
		*best = *current;
}

static uint32_t find_best_rate(const uint32_t *rates, uint32_t n_rates, uint32_t rate, uint32_t def)
{
	uint32_t i, limit;
	struct rate_info best;
	struct rate_info info[n_rates];

	for (i = 0; i < n_rates; i++) {
		info[i].rate = rates[i];
		info[i].gcd = calc_gcd(rate, rates[i]);
		info[i].diff = SPA_ABS((int32_t)rate - (int32_t)rates[i]);
	}

	/* first find higher nearest GCD. This tries to find next bigest rate that
	 * requires the least amount of resample filter banks. Usually these are
	 * rates that are multiples of each other or multiples of a common rate.
	 *
	 * 44100 and [ 32000 56000 88200 96000 ]  -> 88200
	 * 48000 and [ 32000 56000 88200 96000 ]  -> 96000
	 * 88200 and [ 44100 48000 96000 192000 ]  -> 96000
	 * 32000 and [ 44100 192000 ] -> 44100
	 * 8000 and [ 44100 48000 ] -> 48000
	 * 8000 and [ 44100 192000 ] -> 44100
	 * 11025 and [ 44100 48000 ] -> 44100
	 * 44100 and [ 48000 176400 ] -> 48000
	 * 144 and [ 44100 48000 88200 96000] -> 48000
	 */
	spa_zero(best);
	/* Don't try to do excessive upsampling by limiting the max rate
	 * for desired < default to default*2. For other rates allow
	 * a x3 upsample rate max. For values lower than half of the default,
	 * limit to the default.  */
	limit = rate < def/2 ? def : rate < def ? def*2 : rate*3;
	for (i = 0; i < n_rates; i++) {
		if (info[i].rate >= rate && info[i].rate <= limit)
			update_nearest_gcd(&best, &info[i]);
	}
	if (best.rate != 0)
		return best.rate;

	/* we would need excessive upsampling, pick a nearest higher rate */
	spa_zero(best);
	for (i = 0; i < n_rates; i++) {
		if (info[i].rate >= rate)
			update_nearest_rate(&best, &info[i]);
	}
	if (best.rate != 0)
		return best.rate;

	/* There is nothing above the rate, we need to downsample. Try to downsample
	 * but only to something that is from a common rate family. Also don't
	 * try to downsample to something that will sound worse (< 44100).
	 *
	 * 88200 and [ 22050 44100 48000 ] -> 44100
	 * 88200 and [ 22050 48000 ] -> 48000
	 */
	spa_zero(best);
	for (i = 0; i < n_rates; i++) {
		if (info[i].rate >= 44100)
			update_nearest_gcd(&best, &info[i]);
	}
	if (best.rate != 0)
		return best.rate;

	/* There is nothing to downsample above our threshold. Downsample to whatever
	 * is the highest rate then. */
	spa_zero(best);
	for (i = 0; i < n_rates; i++)
		update_highest_rate(&best, &info[i]);
	if (best.rate != 0)
		return best.rate;

	return def;
}

/* here we evaluate the complete state of the graph.
 *
 * It roughly operates in 4 stages:
 *
 * 1. go over all nodes and check if they should be scheduled (runnable) or not.
 *
 * 2. go over all drivers and collect the nodes that need to be scheduled with the
 *    driver. This include all nodes that have an active link with the driver or
 *    with a node already scheduled with the driver.
 *
 * 3. go over all nodes that are not assigned to a driver. The ones that require
 *    a driver are moved to some random active driver found in step 2.
 *
 * 4. go over all drivers again, collect the quantum/rate of all followers, select
 *    the desired final value and activate the followers and then the driver.
 *
 * A complete graph evaluation is performed for each change that is made to the
 * graph, such as making/destroying links, adding/removing nodes, property changes such
 * as quantum/rate changes or metadata changes.
 */
static void context_recalc_graph(void *data)
{
	struct impl *impl = data;
	struct pw_context *context = impl->context;
	struct settings *settings = &context->settings;
	struct pw_impl_node *n, *s, *target, *fallback;
	const uint32_t *rates;
	uint32_t max_quantum, min_quantum, def_quantum, rate_quantum, floor_quantum, ceil_quantum;
	uint32_t n_rates, def_rate, transport;
	bool freewheel, global_force_rate, global_force_quantum;
	struct spa_list collect;

again:
	freewheel = false;

	/* clean up the flags first */
	spa_list_for_each(n, &context->node_list, link) {
		n->visited = false;
		n->checked = 0;
		n->runnable = false;
	}

	get_quantums(context, &def_quantum, &min_quantum, &max_quantum, &rate_quantum,
			&floor_quantum, &ceil_quantum);
	rates = get_rates(context, &def_rate, &n_rates, &global_force_rate);

	global_force_quantum = rate_quantum == 0;

	/* first look at all nodes and decide which one should be runnable */
	spa_list_for_each(n, &context->node_list, link) {
		/* we don't check drivers, they need to be made runnable
		 * from other nodes */
		if (n->exported || !n->active || n->driver)
			continue;
		check_runnable(context, n);
	}

	/* start from all drivers and group all nodes that are linked
	 * to it. Some nodes are not (yet) linked to anything and they
	 * will end up 'unassigned' to a driver. Other nodes are drivers
	 * and if they have active followers, we can use them to schedule
	 * the unassigned nodes. */
	target = fallback = NULL;
	spa_list_for_each(n, &context->driver_list, driver_link) {
		if (n->exported)
			continue;

		if (!n->visited) {
			spa_list_init(&collect);
			collect_nodes(context, n, &collect);
			move_to_driver(context, &collect, n);
		}
		/* from now on we are only interested in active driving nodes
		 * with a driver_priority. We're going to see if there are
		 * active followers. */
		if (!n->driving || !n->active || n->priority_driver <= 0)
			continue;

		/* first active driving node is fallback */
		if (fallback == NULL)
			fallback = n;

		if (!n->runnable)
			continue;

		spa_list_for_each(s, &n->follower_list, follower_link) {
			pw_log_debug("%p: driver %p: follower %p %s: active:%d",
					context, n, s, s->name, s->active);
			if (s != n && s->active) {
				/* if the driving node has active followers, it
				 * is a target for our unassigned nodes */
				if (target == NULL)
					target = n;
				if (n->freewheel)
					freewheel = true;
				break;
			}
		}
	}
	/* no active node, use fallback driving node */
	if (target == NULL)
		target = fallback;

	/* update the freewheel status */
	pw_context_set_freewheel(context, freewheel);

	/* now go through all available nodes. The ones we didn't visit
	 * in collect_nodes() are not linked to any driver. We assign them
	 * to either an active driver or the first driver if they are in a
	 * group that needs a driver. Else we remove them from a driver
	 * and stop them. */
	spa_list_for_each(n, &context->node_list, link) {
		struct pw_impl_node *t, *driver;

		if (n->exported || n->visited)
			continue;

		pw_log_debug("%p: unassigned node %p: '%s' active:%d want_driver:%d target:%p",
				context, n, n->name, n->active, n->want_driver, target);

		/* collect all nodes in this group */
		spa_list_init(&collect);
		collect_nodes(context, n, &collect);

		driver = NULL;
		spa_list_for_each(t, &collect, sort_link) {
			/* is any active and want a driver */
			if ((t->want_driver && t->active && t->runnable) ||
			    t->always_process) {
				driver = target;
				break;
			}
		}
		if (driver != NULL) {
			driver->runnable = true;
			/* driver needed for this group */
			move_to_driver(context, &collect, driver);
		} else {
			/* no driver, make sure the nodes stop */
			remove_from_driver(context, &collect);
		}
	}

	/* assign final quantum and set state for followers and drivers */
	spa_list_for_each(n, &context->driver_list, driver_link) {
		bool running = false, lock_quantum = false, lock_rate = false;
		struct spa_fraction latency = SPA_FRACTION(0, 0);
		struct spa_fraction max_latency = SPA_FRACTION(0, 0);
		struct spa_fraction rate = SPA_FRACTION(0, 0);
		uint32_t target_quantum, target_rate, current_rate, current_quantum;
		uint64_t quantum_stamp = 0, rate_stamp = 0;
		bool force_rate, force_quantum, restore_rate = false, restore_quantum = false;
		bool do_reconfigure = false, need_resume, was_target_pending;
		bool have_request = false;
		const uint32_t *node_rates;
		uint32_t node_n_rates, node_def_rate;
		uint32_t node_max_quantum, node_min_quantum, node_def_quantum, node_rate_quantum;

		if (!n->driving || n->exported)
			continue;

		node_def_quantum = def_quantum;
		node_min_quantum = min_quantum;
		node_max_quantum = max_quantum;
		node_rate_quantum = rate_quantum;
		force_quantum = global_force_quantum;

		node_def_rate = def_rate;
		node_n_rates = n_rates;
		node_rates = rates;
		force_rate = global_force_rate;

		/* collect quantum and rate */
		spa_list_for_each(s, &n->follower_list, follower_link) {

			if (!s->moved) {
				/* We only try to enforce the lock flags for nodes that
				 * are not recently moved between drivers. The nodes that
				 * are moved should try to enforce their quantum on the
				 * new driver. */
				lock_quantum |= s->lock_quantum;
				lock_rate |= s->lock_rate;
			}
			if (!global_force_quantum && s->force_quantum > 0 &&
			    s->stamp > quantum_stamp) {
				node_def_quantum = node_min_quantum = node_max_quantum = s->force_quantum;
				node_rate_quantum = 0;
				quantum_stamp = s->stamp;
				force_quantum = true;
			}
			if (!global_force_rate && s->force_rate > 0 &&
			    s->stamp > rate_stamp) {
				node_def_rate = s->force_rate;
				node_n_rates = 1;
				node_rates = &s->force_rate;
				force_rate = true;
				rate_stamp = s->stamp;
			}

			/* smallest latencies */
			if (latency.denom == 0 ||
			    (s->latency.denom > 0 &&
			     fraction_compare(&s->latency, &latency) < 0))
				latency = s->latency;
			if (max_latency.denom == 0 ||
			    (s->max_latency.denom > 0 &&
			     fraction_compare(&s->max_latency, &max_latency) < 0))
				max_latency = s->max_latency;

			/* largest rate, which is in fact the smallest fraction */
			if (rate.denom == 0 ||
			    (s->rate.denom > 0 &&
			     fraction_compare(&s->rate, &rate) < 0))
				rate = s->rate;

			if (s->active)
				running = n->runnable;

			pw_log_debug("%p: follower %p running:%d runnable:%d rate:%u/%u latency %u/%u '%s'",
				context, s, running, s->runnable, rate.num, rate.denom,
				latency.num, latency.denom, s->name);

			if (running && s != n && s->supports_request > 0)
				have_request = true;

			s->moved = false;
		}

		if (n->forced_rate && !force_rate && n->runnable) {
			/* A node that was forced to a rate but is no longer being
			 * forced can restore its rate */
			pw_log_info("(%s-%u) restore rate", n->name, n->info.id);
			restore_rate = true;
		}
		if (n->forced_quantum && !force_quantum && n->runnable) {
			/* A node that was forced to a quantum but is no longer being
			 * forced can restore its quantum */
			pw_log_info("(%s-%u) restore quantum", n->name, n->info.id);
			restore_quantum = true;
		}

		if (force_quantum)
			lock_quantum = false;
		if (force_rate)
			lock_rate = false;

		need_resume = n->need_resume;
		if (need_resume) {
			running = true;
			n->need_resume = false;
		}

		current_rate = n->target_rate.denom;
		if (!restore_rate &&
		   (lock_rate || need_resume || !running ||
		    (!force_rate && (n->info.state > PW_NODE_STATE_IDLE)))) {
			pw_log_debug("%p: keep rate:1/%u restore:%u lock:%u resume:%u "
					"running:%u force:%u state:%s", context,
					current_rate, restore_rate, lock_rate, need_resume,
					running, force_rate,
					pw_node_state_as_string(n->info.state));

			/* when we don't need to restore or rate and
			 * when someone wants us to lock the rate of this driver or
			 * when we are in the process of reconfiguring the driver or
			 * when we are not running any followers or
			 * when the driver is busy and we don't need to force a rate,
			 * keep the current rate */
			target_rate = current_rate;
		}
		else {
			/* Here we are allowed to change the rate of the driver.
			 * Start with the default rate. If the desired rate is
			 * allowed, switch to it */
			if (rate.denom != 0 && rate.num == 1)
				target_rate = rate.denom;
			else
				target_rate = node_def_rate;

			target_rate = find_best_rate(node_rates, node_n_rates,
						target_rate, node_def_rate);

			pw_log_debug("%p: def_rate:%d target_rate:%d rate:%d/%d", context,
					node_def_rate, target_rate, rate.num, rate.denom);
		}

		was_target_pending = n->target_pending;

		if (target_rate != current_rate) {
			/* we doing a rate switch */
			pw_log_info("(%s-%u) state:%s new rate:%u/(%u)->%u",
					n->name, n->info.id,
					pw_node_state_as_string(n->info.state),
					n->target_rate.denom, current_rate,
					target_rate);

			if (force_rate) {
				if (settings->clock_rate_update_mode == CLOCK_RATE_UPDATE_MODE_HARD)
					do_reconfigure |= !was_target_pending;
			} else {
				if (n->info.state >= PW_NODE_STATE_SUSPENDED)
					do_reconfigure |= !was_target_pending;
			}
			/* we're setting the pending rate. This will become the new
			 * current rate in the next iteration of the graph. */
			n->target_rate = SPA_FRACTION(1, target_rate);
			n->forced_rate = force_rate;
			n->target_pending = true;
			current_rate = target_rate;
		}

		if (node_rate_quantum != 0 && current_rate != node_rate_quantum) {
			/* the quantum values are scaled with the current rate */
			node_def_quantum = SPA_SCALE32(node_def_quantum, current_rate, node_rate_quantum);
			node_min_quantum = SPA_SCALE32(node_min_quantum, current_rate, node_rate_quantum);
			node_max_quantum = SPA_SCALE32(node_max_quantum, current_rate, node_rate_quantum);
		}

		/* calculate desired quantum. Don't limit to the max_latency when we are
		 * going to force a quantum or rate and reconfigure the nodes. */
		if (max_latency.denom != 0 && !force_quantum && !force_rate) {
			uint32_t tmp = SPA_SCALE32(max_latency.num, current_rate, max_latency.denom);
			if (tmp < node_max_quantum)
				node_max_quantum = tmp;
		}

		current_quantum = n->target_quantum;
		if (!restore_quantum && (lock_quantum || need_resume || !running)) {
			pw_log_debug("%p: keep quantum:%u restore:%u lock:%u resume:%u "
					"running:%u force:%u state:%s", context,
					current_quantum, restore_quantum, lock_quantum, need_resume,
					running, force_quantum,
					pw_node_state_as_string(n->info.state));
			target_quantum = current_quantum;
		}
		else {
			target_quantum = node_def_quantum;
			if (latency.denom != 0)
				target_quantum = SPA_SCALE32(latency.num, current_rate, latency.denom);
			target_quantum = SPA_CLAMP(target_quantum, node_min_quantum, node_max_quantum);
			target_quantum = SPA_CLAMP(target_quantum, floor_quantum, ceil_quantum);

			if (settings->clock_power_of_two_quantum && !force_quantum)
				target_quantum = flp2(target_quantum);
		}

		if (target_quantum != current_quantum) {
			pw_log_info("(%s-%u) new quantum:%"PRIu64"->%u",
					n->name, n->info.id,
					n->target_quantum,
					target_quantum);
			/* this is the new pending quantum */
			n->target_quantum = target_quantum;
			n->forced_quantum = force_quantum;
			n->target_pending = true;

			if (force_quantum)
				do_reconfigure |= !was_target_pending;
		}

		if (n->target_pending) {
			if (do_reconfigure) {
				reconfigure_driver(context, n);
				/* we might be suspended now and the links need to be prepared again */
				goto again;
			}
			/* we have a pending change. We place the new values in the
			 * pending fields so that they are picked up by the driver in
			 * the next cycle */
			pw_log_debug("%p: apply duration:%"PRIu64" rate:%u/%u", context,
					n->target_quantum, n->target_rate.num,
					n->target_rate.denom);
			SPA_SEQ_WRITE(n->rt.position->clock.target_seq);
			n->rt.position->clock.target_duration = n->target_quantum;
			n->rt.position->clock.target_rate = n->target_rate;
			SPA_SEQ_WRITE(n->rt.position->clock.target_seq);

			if (n->info.state < PW_NODE_STATE_RUNNING) {
				n->rt.position->clock.duration = n->target_quantum;
				n->rt.position->clock.rate = n->target_rate;
			}
			n->target_pending = false;
		} else {
			n->target_quantum = n->rt.position->clock.target_duration;
			n->target_rate = n->rt.position->clock.target_rate;
		}

		if (n->info.state < PW_NODE_STATE_RUNNING)
			n->rt.position->clock.nsec = get_time_ns(n->rt.target.system);

		SPA_FLAG_UPDATE(n->rt.position->clock.flags,
				SPA_IO_CLOCK_FLAG_LAZY, have_request && n->supports_lazy > 0);

		pw_log_debug("%p: driver %p running:%d runnable:%d quantum:%u rate:%u (%"PRIu64"/%u)'%s'",
				context, n, running, n->runnable, target_quantum, target_rate,
				n->rt.position->clock.target_duration,
				n->rt.position->clock.target_rate.denom, n->name);

		transport = PW_NODE_ACTIVATION_COMMAND_NONE;

		/* first change the node states of the followers to the new target */
		spa_list_for_each(s, &n->follower_list, follower_link) {
			if (s->transport != PW_NODE_ACTIVATION_COMMAND_NONE) {
				transport = s->transport;
				s->transport = PW_NODE_ACTIVATION_COMMAND_NONE;
			}
			if (s == n)
				continue;
			pw_log_debug("%p: follower %p: active:%d '%s'",
					context, s, s->active, s->name);
			ensure_state(s, running);
		}

		if (transport != PW_NODE_ACTIVATION_COMMAND_NONE) {
			pw_log_info("%s: transport %d", n->name, transport);
			SPA_ATOMIC_STORE(n->rt.target.activation->command, transport);
		}

		/* now that all the followers are ready, start the driver */
		ensure_state(n, running);
	}
}

static const struct pw_context_events context_events = {
	PW_VERSION_CONTEXT_EVENTS,
	.recalc_graph = context_recalc_graph,
};

static void module_destroy(void *data)
{
	struct impl *impl = data;

	if (impl->context) {
		spa_hook_remove(&impl->context_listener);
		spa_hook_remove(&impl->module_listener);
	}

	pw_properties_free(impl->props);

	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args_str)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *args;
	struct impl *impl;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args_str);

	if (args_str)
		args = pw_properties_new_string(args_str);
	else
		args = pw_properties_new(NULL, NULL);

	if (!args) {
		res = -errno;
		goto error;
	}

	pw_context_conf_update_props(context, "module."NAME".args", args);

	impl->props = args;
	impl->context = context;

	pw_context_add_listener(context, &impl->context_listener, &context_events, impl);
	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	module_destroy(impl);
	return res;
}
