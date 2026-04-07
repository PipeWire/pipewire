# AVB Module Bugs Found via Test Suite

The following bugs were discovered by building a software test harness
for the AVB protocol stack. All have been fixed in the accompanying
patch series.

## 1. Heap corruption in server_destroy_descriptors

**File:** `src/modules/module-avb/internal.h`
**Commit:** `69c721006`

`server_destroy_descriptors()` called `free(d->ptr)` followed by
`free(d)`, but `d->ptr` points into the same allocation as `d`
(set via `SPA_PTROFF(d, sizeof(struct descriptor), void)` in
`server_add_descriptor()`). This is a double-free / heap corruption
that could cause crashes or memory corruption when tearing down an
AVB server.

**Fix:** Remove the erroneous `free(d->ptr)` call.

## 2. NULL pointer dereference in MSRP notify dispatch

**File:** `src/modules/module-avb/msrp.c`, `src/modules/module-avb/mvrp.c`
**Commit:** `b056e9f85`

`msrp_notify()` unconditionally calls `dispatch[a->attr.type].notify()`
but the dispatch table entry for `AVB_MSRP_ATTRIBUTE_TYPE_TALKER_FAILED`
has `notify = NULL`. If a talker-failed attribute receives a registrar
state change (e.g., `RX_NEW` triggers `NOTIFY_NEW`), this crashes with
a NULL pointer dereference. The same unguarded pattern exists in
`mvrp_notify()`.

**Fix:** Add `if (dispatch[a->attr.type].notify)` NULL check before
calling, matching the defensive pattern already used in the encode path.

## 3. MRP NEW messages never transmitted

**File:** `src/modules/module-avb/mrp.h`, `src/modules/module-avb/mrp.c`,
`src/modules/module-avb/msrp.c`, `src/modules/module-avb/mvrp.c`
**Commit:** `bc2c41daa`

`AVB_MRP_SEND_NEW` was defined as `0`. The MSRP and MVRP event handlers
skip attributes with `if (!a->attr.mrp->pending_send)`, treating `0` as
"no pending send". Since the MRP state machine sets `pending_send` to
`AVB_MRP_SEND_NEW` (0) when an attribute in state VN or AN receives a
TX event, NEW messages were silently dropped instead of being
transmitted. This violates IEEE 802.1Q which requires NEW messages to
be sent when an attribute is first declared.

In practice, the attribute would cycle through VN -> AN -> AA over
successive TX events, eventually sending a JOINMT instead of the
initial NEW. The protocol still functioned because JOINMT also
registers the attribute, but the initial declaration was lost.

**Fix:** Shift all `AVB_MRP_SEND_*` values to start at 1, so that 0
unambiguously means "no send pending". Update MSRP and MVRP encoders
to subtract 1 when encoding to the IEEE 802.1Q wire format.

## 4. ACMP error responses sent with wrong message type

**File:** `src/modules/module-avb/acmp.c`
**Commit:** `9f4147104`

In `handle_connect_tx_command()` and `handle_disconnect_tx_command()`,
`AVB_PACKET_ACMP_SET_MESSAGE_TYPE()` is called after the `goto done`
jump target. When `find_stream()` fails (returns NULL), the code jumps
to `done:` without setting the message type, so the error response is
sent with the original command message type (e.g.,
`CONNECT_TX_COMMAND = 0`) instead of the correct response type
(`CONNECT_TX_RESPONSE = 1`).

A controller receiving this malformed response would not recognize it
as a response to its command and would eventually time out.

**Fix:** Move `AVB_PACKET_ACMP_SET_MESSAGE_TYPE()` before the
`find_stream()` call so the response type is always set correctly.

## 5. ACMP pending_destroy skips controller cleanup

**File:** `src/modules/module-avb/acmp.c`
**Commit:** `9f4147104`

`pending_destroy()` iterates with `list_id < PENDING_CONTROLLER`
(where `PENDING_CONTROLLER = 2`), which only cleans up
`PENDING_TALKER` (0) and `PENDING_LISTENER` (1) lists, skipping
`PENDING_CONTROLLER` (2). Any pending controller requests leak on
shutdown.

**Fix:** Change `<` to `<=` to include the controller list.
