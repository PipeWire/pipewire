/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Canonical Ltd. */
/* SPDX-License-Identifier: MIT */

#ifndef _SNAP_POLICY_H_
#define _SNAP_POLICY_H_

typedef enum _pw_sandbox_access {
	PW_SANDBOX_ACCESS_NONE           = 0,
	PW_SANDBOX_ACCESS_NOT_A_SANDBOX  = 1 << 0,
	PW_SANDBOX_ACCESS_RECORD         = 1 << 1,
	PW_SANDBOX_ACCESS_PLAYBACK       = 1 << 2,
	PW_SANDBOX_ACCESS_ALL            = (PW_SANDBOX_ACCESS_PLAYBACK | PW_SANDBOX_ACCESS_RECORD),
} pw_sandbox_access_t;

#define PW_KEY_SNAP_ID "pipewire.snap.id"
#define PW_KEY_SNAP_PLAYBACK_ALLOWED "pipewire.snap.audio.playback"
#define PW_KEY_SNAP_RECORD_ALLOWED "pipewire.snap.audio.record"

pw_sandbox_access_t pw_snap_get_audio_permissions(struct client *client, int fd, char **app_id);

#endif // _SNAP_POLICY_H_
