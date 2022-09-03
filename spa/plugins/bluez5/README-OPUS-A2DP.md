---
title: OPUS-A2DP-0.5 specification
author: Pauli Virtanen <pav@iki.fi>
date: Jun 4, 2022
---

# OPUS-A2DP-0.5 specification

In this file, a way to use Opus as an A2DP vendor codec is specified.

We will call this "OPUS-A2DP-0.5". There is no previous public
specification for using Opus as an A2DP vendor codec (to my
knowledge), which is why we need this one.

[[_TOC_]]

# Media Codec Capabilities

The Media Codec Specific Information Elements ([AVDTP v1.3], §8.21.5)
capability and configuration structure is as follows:

| Octet | Bits | Meaning                                       |
|-------|------|-----------------------------------------------|
| 0-5   | 0-7  | Vendor ID Part                                |
| 6-7   | 0-7  | Channel Configuration                         |
| 8-11  | 0-7  | Audio Location Configuration                  |
| 12-14 | 0-7  | Limits Configuration                          |
| 15-16 | 0-7  | Return Direction Channel Configuration        |
| 17-20 | 0-7  | Return Direction Audio Location Configuration |
| 21-23 | 0-7  | Return Direction Limits Configuration         |

All integer fields and multi-byte bitfields are laid out in **little
endian** order.  All integer fields are unsigned.

Each entry may have different meaning when present as a capability.
Below, we indicate this by abbreviations CAP for capability and SEL
for the value selected by SRC.

Bits in fields marked RFA (Reserved For Additions) shall be set to
zero.

> **Note**
>
> See `a2dp-codec-caps.h` for definition as C structs.

## Vendor ID Part

The fixed value

| Octet | Bits | Meaning                       |
|-------|------|-------------------------------|
| 0-3   | 0-7  | A2DP Vendor ID (0x05F1)       |
| 4-5   | 0-7  | A2DP Vendor Codec ID (0x1005) |

> **Note**
>
> The Vendor ID is that of the Linux Foundation, and we are using it
> here unofficially.

## Channel Configuration

The channel configuration consists of the channel count, and the count
of coupled streams. The latter indicates which channels are encoded as
left/right pairs, as defined in Sec. 5.1.1 of Opus Ogg Encapsulation [RFC7845].

| Octet | Bits | Meaning                                                    |
|-------|------|------------------------------------------------------------|
| 6     | 0-7  | Channel Count. CAP: maximum number supported. SEL: actual. |
| 7     | 0-7  | Coupled Stream Count. CAP: 0. SEL: actual.                 |

The Channel Count indicates the number of logical channels encoded in
the data stream.

The Coupled Stream Count indicates the number of streams that encode a
coupled (left & right) channel pair.  The count shall satisfy
`(Channel Count) >= 2*(Coupled Stream Count)`.
The Stream Count is `(Channel Count) - (Coupled Stream Count)`.

The logical Channels are identified by a Channel Index *j* such that `0 <= j
< (Channel Count)`. The channels `0 <= j < 2*(Coupled Stream Count)`
are encoded in the *k*-th stream of the payload, where `k = floor(j/2)` and
`j mod 2` determines which of the two channels of the stream the logical
channel is. The channels `2*(Coupled Stream Count) <= j < (Channel Count)`
are encoded in the *k*-th stream of the payload, where `k = j - (Coupled Stream Count)`.

> **Note**
>
> The prescription here is identical to [RFC7845] with channel mapping
> `mapping[j] = j`. We do not want to include the mapping table in the
> A2DP capabilities, so it is assumed to be trivial.

## Audio Location Configuration

The semantic meaning for each channel is determined by their Audio
Location bitfield.

| Octet | Bits | Meaning                                              |
|-------|------|------------------------------------------------------|
| 8-11  | 0-7  | Audio Location bitfield. CAP: available. SEL: actual |

The values specified in CAP are informative, and SEL may contain bits
that were not set in CAP. SNK shall handle unsupported audio
locations. It may do this for example by ignoring unsupported channels
or via suitable up/downmixing.  Hence, SRC may transmit channels with
audio locations that are not marked supported by SNK.

The audio location bit values are:

| Channel Order | Bitmask    | Audio Location          |
|---------------|------------|-------------------------|
| 0             | 0x00000001 | Front Left              |
| 1             | 0x00000002 | Front Right             |
| 2             | 0x00000400 | Side Left               |
| 3             | 0x00000800 | Side Right              |
| 4             | 0x00000010 | Back Left               |
| 5             | 0x00000020 | Back Right              |
| 6             | 0x00000040 | Front Left of Center    |
| 7             | 0x00000080 | Front Right of Center   |
| 8             | 0x00001000 | Top Front Left          |
| 9             | 0x00002000 | Top Front Right         |
| 10            | 0x00040000 | Top Side Left           |
| 11            | 0x00080000 | Top Side Right          |
| 12            | 0x00010000 | Top Back Left           |
| 13            | 0x00020000 | Top Back Right          |
| 14            | 0x00400000 | Bottom Front Left       |
| 15            | 0x00800000 | Bottom Front Right      |
| 16            | 0x01000000 | Front Left Wide         |
| 17            | 0x02000000 | Front Right Wide        |
| 18            | 0x04000000 | Left Surround           |
| 19            | 0x08000000 | Right Surround          |
| 20            | 0x00000004 | Front Center            |
| 21            | 0x00000100 | Back Center             |
| 22            | 0x00004000 | Top Front Center        |
| 23            | 0x00008000 | Top Center              |
| 24            | 0x00100000 | Top Back Center         |
| 25            | 0x00200000 | Bottom Front Center     |
| 26            | 0x00000008 | Low Frequency Effects 1 |
| 27            | 0x00000200 | Low Frequency Effects 2 |
| 28            | 0x10000000 | RFA                     |
| 29            | 0x20000000 | RFA                     |
| 30            | 0x40000000 | RFA                     |
| 31            | 0x80000000 | RFA                     |

Each bit value is associated with a Channel Order.  The bits set in
the bitfield define audio locations for the streams present in the
payload. The set bit with the smallest Channel Order value defines the
audio location for the Channel Index *j=0*, the bit with the next
lowest Channel Order value defines the audio location for the Channel
Index *j=1*, and so forth.

When the Channel Count is larger than the number of bits set in the
Audio Location bitfield, the audio locations of the remaining channels
are unspecified. Implementations may handle them as appropriate for
their use case, considering them as AUX0–AUXN, or in the case of
Channel Count = 1, as the single mono audio channel.

When the Channel Count is smaller than the number of bits set in the
Audio Location bitfield, the audio locations for the channels are
assigned as above, and remaining excess bits shall be ignored.

> **Note**
>
> The channel audio location specification is similar to the location
> bitfield of the `Audio_Channel_Allocation` LTV structure in Bluetooth
> SIG [Assigned Numbers, Generic Audio] used in the LE Audio, and the
> bitmasks defined above are the same.
>
> The channel ordering differs from LE Audio, and is defined here to be
> compatible with the internal stream ordering in the reference Opus
> Multistream surround encoder Mapping Family 0 and 1 output. This
> allows making use of its surround masking and LFE handling
> capabilities.  The stream ordering of the reference Opus surround
> encoder, although being unchanged since its addition in 2013, is an
> internal detail of the encoder. Implementations using the surround
> encoder need to check that the mapping table used by the encoder
> corresponds to the above channel ordering.
>
> For reference, we list the Audio Location bitfield values
> corresponding to the different channel counts in Opus Mapping Family 0
> and 1 surround encoder output, and the expected mapping table:
>
> | Mapping Family | Channel Count | Audio Location Value | Stream Ordering                 | Mapping Table            |
> |----------------|---------------|----------------------|---------------------------------|--------------------------|
> | 0              | 1             | 0x00000000           | mono                            | {0}                      |
> | 0              | 2             | 0x00000003           | FL, FR                          | {0, 1}                   |
> | 1              | 1             | 0x00000000           | mono                            | {0}                      |
> | 1              | 2             | 0x00000003           | FL, FR                          | {0, 1}                   |
> | 1              | 3             | 0x00000007           | FL, FR, FC                      | {0, 2, 1}                |
> | 1              | 4             | 0x00000033           | FL, FR, BL, BR                  | {0, 1, 2, 3}             |
> | 1              | 5             | 0x00000037           | FL, FR, BL, BR, FC              | {0, 4, 1, 2, 3}          |
> | 1              | 6             | 0x0000003f           | FL, FR, BL, BR, FC, LFE         | {0, 4, 1, 2, 3, 5}       |
> | 1              | 7             | 0x00000d0f           | FL, FR, SL, SR, FC, BC, LFE     | {0, 4, 1, 2, 3, 5, 6}    |
> | 1              | 8             | 0x00000c3f           | FL, FR, SL, SR, BL, BR, FC, LFE | {0, 6, 1, 2, 3, 4, 5, 7} |
>
> The Mapping Table in the table indicates the mapping table selected by
> `opus_multistream_surround_encoder_create` (Opus 1.3.1). If the
> encoder outputs a different mapping table in a future Opus encoder
> release, the channel ordering will be incorrect, and the surround
> encoder can not be used. We expect that the probability of the Opus
> encoder authors making such changes is negligible.

## Limits Configuration

The limits for allowed frame durations and maximum bitrate can also be
configured.

| Octet | Bits | Meaning                                             |
|-------|------|-----------------------------------------------------|
| 16    | 0    | Frame duration 2.5ms. CAP: supported, SEL: selected |
| 16    | 1    | Frame duration 5ms. CAP: supported, SEL: selected   |
| 16    | 2    | Frame duration 10ms. CAP: supported, SEL: selected  |
| 16    | 3    | Frame duration 20ms. CAP: supported, SEL: selected  |
| 16    | 4    | Frame duration 40ms. CAP: supported, SEL: selected  |
| 16    | 5-7  | RFA                                                 |

| Octet | Bits | Meaning                                        |
|-------|------|------------------------------------------------|
| 17-18 | 0-7  | Maximum bitrate. CAP: supported, SEL: selected |

The maximum bitrate is given in units of 1024 bits per second.

The maximum bitrate field in CAP may contain value 0 to indicate
everything is supported.

## Bidirectional Audio Configuration

Bidirectional audio may be supported. Its Channel Configuration, Audio
Location Configuration, and Limits Configuration have identical form
to the forward direction, and represented by exactly similar
structures.

Namely:

| Octet | Bits | Meaning                                            |
|-------|------|----------------------------------------------------|
| 19-20 | 0-7  | Channel Configuration fields, for return direction |
| 21-28 | 0-7  | Audio Location fields, for return direction        |
| 29-31 | 0-7  | Limits Configuration fields, for return direction  |

If no return channel is supported or selected, the number of channels
is set to 0 in CAP or SEL.

> **Note**
>
> This is a nonstandard extension to A2DP. The return direction audio
> data is simply sent back via the underlying L2CAP connection, which
> is bidirectional, in the same format as the forward direction audio.
> This is similar to what aptX-LL and FastStream do.

# Packet Structure

Each packet consists of an RTP header, an RTP payload header, and a
payload containing Opus Multistream data.

| Octet | Bits | Meaning                  |
|-------|------|--------------------------|
| 0-11  | 0-7  | RTP header               |
| 12    | 0-7  | RTP payload header       |
| 13-N  | 0-7  | Opus Multistream payload |

For each Bluetooth packet, the payload shall contain exactly one Opus
Multistream packet, or a fragment of one. The Opus Multistream packet
may be fragmented to several consecutive Bluetooth packets.

The format of the Multistream data is the same as in the audio packets
of [RFC7845], or, as produced/consumed by the Opus Multistream API.

> **Note**
>
> We DO NOT follow [RFC7587], as we want fragmentation and multichannel support.

## RTP Header

See [RFC3550].

The RTP payload type is pt=96 (dynamic).

## RTP Payload Header

The RTP payload header is used to indicate if and how the Opus
Multistream packet is fragmented across several consecutive Bluetooth
packets.

| Octet  | Bits | Meaning
|--------|------|--------------------------------------------------------
|   0    | 0-3  | Frame Count
|   4    | 4    | RFA
|   4    | 5    | Is Last Fragment
|   4    | 6    | Is First Fragment
|   4    | 7    | Is Fragmented

In each packet, Frame Count indicates how many Bluetooth packets are
still to be received (including the present packet) before the Opus
Multistream packet is complete.

The Is Fragment flag indicates whether the present packet contains
fragmented payload.

The Is Last Fragment flag indicates whether the present packet is the
last part of fragmented payload.

The Is First Fragment flag indicates whether the present packet is the
first part of fragmented payload.

In non-fragmented packets, Frame Count shall be (1), and the other bits
in the header zero.

## Opus Payload

The Opus payload is a single Opus Multistream packet, or its fragment.

In case of fragmentation, as indicated by the RTP payload header,
concatenating the payloads of the fragment Bluetooth packets shall
yield the total Opus Multistream packet.

The SRC should choose encoder parameters such that Bluetooth bandwidth
limitations are not exceeded.

The SRC may include FEC data. The SNK may enable forward error
correction instead of PLC.


# References

1. Bluetooth [AVDTP v1.3]
2. IETF [RFC3550]
3. IETF [RFC7587]
4. IETF [RFC7845]
5. Bluetooth [Assigned Numbers, Generic Audio]

[AVDTP v1.3]: https://www.bluetooth.com/specifications/specs/a-v-distribution-transport-protocol-1-3/
[RFC3550]: https://datatracker.ietf.org/doc/html/rfc3550
[RFC7587]: https://datatracker.ietf.org/doc/html/rfc7587
[RFC7845]: https://datatracker.ietf.org/doc/html/rfc7845
[Assigned Numbers, Generic Audio]: https://www.bluetooth.com/specifications/assigned-numbers/
