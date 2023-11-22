/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Canonical Ltd. */
/* SPDX-License-Identifier: MIT */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <snapd-glib/snapd-glib.h>
#include <pipewire/pipewire.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "client.h"
#include <sys/apparmor.h>
#include <errno.h>
#include "snap-policy.h"
#include <fcntl.h>
#include <assert.h>

#define SNAP_LABEL_PREFIX      "snap."

static gboolean check_is_same_snap(gchar *snap1, gchar *snap2)
{
    // Checks if two apparmor labels belong to the same snap
    g_auto(GStrv) strings1 = NULL;
    g_auto(GStrv) strings2 = NULL;

    if (!g_str_has_prefix(snap1, SNAP_LABEL_PREFIX)) {
        return FALSE;
    }
    if (!g_str_has_prefix(snap2, SNAP_LABEL_PREFIX)) {
        return FALSE;
    }
    strings1 = g_strsplit(snap1, ".", 3);
    strings2 = g_strsplit(snap2, ".", 3);

    if (g_str_equal(strings1[1], strings2[1]) && (strings1[1] != NULL)) {
        return TRUE;
    }
    return FALSE;
}

pw_sandbox_access_t pw_snap_get_audio_permissions(struct client *client, int fd, char **app_id)
{
    g_autofree gchar* aa_label = NULL;
    gchar* snap_id = NULL;
    gchar* snap_confinement = NULL;
    gchar *separator = NULL;
    g_autofree gchar *aacon = NULL;
    gchar *aamode = NULL;
    g_autoptr(SnapdClient) snapdclient = NULL;
    g_autoptr(GPtrArray) plugs = NULL;
    gboolean retv;
    pw_sandbox_access_t permissions = PW_SANDBOX_ACCESS_NONE;
    SnapdPlug **plug = NULL;
    g_autoptr(GError) error = NULL;
    int exit_code;

    *app_id = g_strdup("unknown");
    assert(client != NULL);

    if (aa_getpeercon(fd, &aa_label, &snap_confinement) == -1) {
        if (errno == EINVAL) {
            // if apparmor isn't enabled, we can safely assume that there are no SNAPs in the system
            return PW_SANDBOX_ACCESS_NOT_A_SANDBOX;
        }
        pw_log_warn("snap_get_audio_permissions: failed to get the AppArmor info.");
        return PW_SANDBOX_ACCESS_NONE;
    }
    if (!g_str_has_prefix(aa_label, SNAP_LABEL_PREFIX)) {
        // not a SNAP.
        pw_log_info("snap_get_audio_permissions: not an snap.");
        return PW_SANDBOX_ACCESS_NOT_A_SANDBOX;
    }

    snap_id = g_strdup(aa_label + strlen(SNAP_LABEL_PREFIX));
    separator = strchr(snap_id, '.');
    if (separator == NULL) {
        pw_log_info("snap_get_audio_permissions: aa_label has only one dot; not a valid ID.");
        return PW_SANDBOX_ACCESS_NONE;
    }
    *separator = 0;
    g_free(*app_id);
    *app_id = snap_id;

    // it's a "classic" or a "devmode" confinement snap, so we give it full access
    if (g_str_equal(snap_confinement, "complain")) {
        return PW_SANDBOX_ACCESS_ALL;
    }

    snapdclient = snapd_client_new();
    if (snapdclient == NULL) {
        pw_log_warn("snap_get_audio_permissions: error creating SnapdClient object.");
        return PW_SANDBOX_ACCESS_NONE;
    }

    if (aa_getcon(&aacon, &aamode) == -1) {
        pw_log_warn("snap_get_audio_permissions: error checking if pipewire-pulse is inside a snap.");
        return PW_SANDBOX_ACCESS_NONE; // failed to get access to apparmor
    }

    // If pipewire-pulse is inside a snap, use snapctl API
    if (g_str_has_prefix(aacon, SNAP_LABEL_PREFIX)) {
        // If the snap wanting to get access is the same that contains pipewire,
        // give to it full access.
        if (check_is_same_snap(aacon, aa_label))
            return PW_SANDBOX_ACCESS_ALL;
        snapd_client_set_socket_path (snapdclient, "/run/snapd-snap.socket");

        /* Take context from the environment if available */
        const char *context = g_getenv ("SNAP_COOKIE");
        if (!context)
            context = "";

        char *snapctl_command[] = { "is-connected", "--apparmor-label", aa_label, "pulseaudio", NULL };
        if (!snapd_client_run_snapctl2_sync (snapdclient, context, (char **) snapctl_command, NULL, NULL, &exit_code, NULL, &error)) {
            pw_log_warn("snap_get_audio_permissions: error summoning snapctl2 for pulseaudio interface: %s", error->message);
            return PW_SANDBOX_ACCESS_NONE;
        }
        if (exit_code != 1) {
            // 0  = Connected
            // 10 = Classic environment
            // 11 = Not a snap
            return PW_SANDBOX_ACCESS_ALL;
        }
        char *snapctl_command2[] = { "is-connected", "--apparmor-label", aa_label, "audio-record", NULL };
        if (!snapd_client_run_snapctl2_sync (snapdclient, context, (char **) snapctl_command2, NULL, NULL, &exit_code, NULL, &error)) {
            pw_log_warn("snap_get_audio_permissions: error summoning snapctl2 for audio-record interface: %s", error->message);
            return PW_SANDBOX_ACCESS_NONE;
        }
        if (exit_code == 1) {
            return PW_SANDBOX_ACCESS_PLAYBACK;
        }
        return PW_SANDBOX_ACCESS_ALL;
    }

    retv = snapd_client_get_connections2_sync(snapdclient,
                                              SNAPD_GET_CONNECTIONS_FLAGS_NONE,
                                              snap_id,
                                              NULL,
                                              NULL,
                                              NULL,
                                              &plugs,
                                              NULL,
                                              NULL,
                                              &error);
    if (retv == FALSE) {
        pw_log_warn("Failed to get Snap connections for snap %s: %s\n", snap_id, error->message);
        return PW_SANDBOX_ACCESS_NONE;
    }
    if (plugs == NULL) {
        pw_log_warn("Failed to get Snap connections for snap %s: %s\n", snap_id, error->message);
        return PW_SANDBOX_ACCESS_NONE;
    }
    if (plugs->pdata == NULL) {
        pw_log_warn("Failed to get Snap connections for snap %s: %s\n", snap_id, error->message);
        return PW_SANDBOX_ACCESS_NONE;
    }

    plug = (SnapdPlug **)plugs->pdata;
    for (guint p = 0; p < plugs->len; p++, plug++) {
        pw_sandbox_access_t add_permission;
        const gchar *plug_name = snapd_plug_get_name(*plug);
        if (g_str_equal("audio-record", plug_name)) {
            add_permission = PW_SANDBOX_ACCESS_RECORD;
        } else if (g_str_equal("audio-playback", plug_name)) {
            add_permission = PW_SANDBOX_ACCESS_PLAYBACK;
        } else if (g_str_equal("pulseaudio", plug_name)) {
            add_permission = PW_SANDBOX_ACCESS_ALL;
        } else {
            continue;
        }
        GPtrArray *slots = snapd_plug_get_connected_slots(*plug);
        if (slots == NULL)
            continue;
        SnapdSlotRef **slot = (SnapdSlotRef**) slots->pdata;

        for (guint q = 0; q < slots->len; q++, slot++) {
            const gchar *slot_name = snapd_slot_ref_get_slot (*slot);
            const gchar *snap_name = snapd_slot_ref_get_snap (*slot);
            if (g_str_equal (snap_name, "snapd") &&
                g_str_equal (slot_name, plug_name))
                    permissions |= add_permission;
        }
    }

    return permissions;
}
