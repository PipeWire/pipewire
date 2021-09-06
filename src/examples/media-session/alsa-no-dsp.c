/* PipeWire
 *
 * Copyright © 2021 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include "pipewire/pipewire.h"
#include "pipewire/extensions/metadata.h"

#include "media-session.h"

/** \page page_media_session_module_no_dsp Media Session Module: No DSP
 *
 * Instruct \ref page_media_session_module_policy_node to not configure audio
 * adapter nodes in DSP mode.  Device nodes will always be configured in
 * passthrough mode. If a client node wants to be linked with a device node
 * that has a different format, then the policy will configure the client node
 * in convert mode so that both nodes have the same format.
 *
 * This is done by just setting a session property flag, and policy-node does the rest.
 */

#define KEY_NAME	"policy-node.alsa-no-dsp"

int sm_alsa_no_dsp_start(struct sm_media_session *session)
{
	pw_properties_set(session->props, KEY_NAME, "true");
	return 0;
}
