/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

#include <spa/utils/string.h>
#include <spa/control/ump-utils.h>

#include "midievent.h"

static const char * const event_names[] = {
	"Text", "Copyright", "Sequence/Track Name",
	"Instrument", "Lyric", "Marker", "Cue Point",
	"Program Name", "Device (Port) Name"
};

static const char * const note_names[] = {
	"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

static const char * const controller_names[128] = {
	[0] =	"Bank Select (coarse)",
	[1] =	"Modulation Wheel (coarse)",
	[2] =	"Breath controller (coarse)",
	[4] =	"Foot Pedal (coarse)",
	[5] =	"Portamento Time (coarse)",
	[6] =	"Data Entry (coarse)",
	[7] =	"Volume (coarse)",
	[8] =	"Balance (coarse)",
	[10] =	"Pan position (coarse)",
	[11] =	"Expression (coarse)",
	[12] =	"Effect Control 1 (coarse)",
	[13] =	"Effect Control 2 (coarse)",
	[16] =	"General Purpose Slider 1",
	[17] =	"General Purpose Slider 2",
	[18] =	"General Purpose Slider 3",
	[19] =	"General Purpose Slider 4",
	[32] =	"Bank Select (fine)",
	[33] =	"Modulation Wheel (fine)",
	[34] =	"Breath 	 (fine)",
	[36] =	"Foot Pedal (fine)",
	[37] =	"Portamento Time (fine)",
	[38] =	"Data Entry (fine)",
	[39] =	"Volume (fine)",
	[40] =	"Balance (fine)",
	[42] =	"Pan position (fine)",
	[43] =	"Expression (fine)",
	[44] =	"Effect Control 1 (fine)",
	[45] =	"Effect Control 2 (fine)",
	[64] =	"Hold Pedal (on/off)",
	[65] =	"Portamento (on/off)",
	[66] =	"Sustenuto Pedal (on/off)",
	[67] =	"Soft Pedal (on/off)",
	[68] =	"Legato Pedal (on/off)",
	[69] =	"Hold 2 Pedal (on/off)",
	[70] =	"Sound Variation",
	[71] =	"Sound Timbre",
	[72] =	"Sound Release Time",
	[73] =	"Sound Attack Time",
	[74] =	"Sound Brightness",
	[75] =	"Sound Control 6",
	[76] =	"Sound Control 7",
	[77] =	"Sound Control 8",
	[78] =	"Sound Control 9",
	[79] =	"Sound Control 10",
	[80] =	"General Purpose Button 1 (on/off)",
	[81] =	"General Purpose Button 2 (on/off)",
	[82] =	"General Purpose Button 3 (on/off)",
	[83] =	"General Purpose Button 4 (on/off)",
	[91] =	"Effects Level",
	[92] =	"Tremulo Level",
	[93] =	"Chorus Level",
	[94] =	"Celeste Level",
	[95] =	"Phaser Level",
	[96] =	"Data Button increment",
	[97] =	"Data Button decrement",
	[98] =	"Non-registered Parameter (fine)",
	[99] =	"Non-registered Parameter (coarse)",
	[100] =	"Registered Parameter (fine)",
	[101] =	"Registered Parameter (coarse)",
	[120] =	"All Sound Off",
	[121] =	"All Controllers Off",
	[122] =	"Local Keyboard (on/off)",
	[123] =	"All Notes Off",
	[124] =	"Omni Mode Off",
	[125] =	"Omni Mode On",
	[126] =	"Mono Operation",
	[127] =	"Poly Operation",
};

static const char * const program_names[] = {
	"Acoustic Grand", "Bright Acoustic", "Electric Grand", "Honky-Tonk",
	"Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
	"Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba",
	"Xylophone", "Tubular Bells", "Dulcimer", "Drawbar Organ", "Percussive Organ",
	"Rock Organ", "Church Organ", "Reed Organ", "Accoridan", "Harmonica",
	"Tango Accordion", "Nylon String Guitar", "Steel String Guitar",
	"Electric Jazz Guitar", "Electric Clean Guitar", "Electric Muted Guitar",
	"Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
	"Acoustic Bass", "Electric Bass (fingered)", "Electric Bass (picked)",
	"Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
	"Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings",
	"Orchestral Strings", "Timpani", "String Ensemble 1", "String Ensemble 2",
	"SynthStrings 1", "SynthStrings 2", "Choir Aahs", "Voice Oohs", "Synth Voice",
	"Orchestra Hit", "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn",
	"Brass Section", "SynthBrass 1", "SynthBrass 2", "Soprano Sax", "Alto Sax",
	"Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
	"Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Skakuhachi",
	"Whistle", "Ocarina", "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)",
	"Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)",
	"Lead 8 (bass+lead)", "Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)",
	"Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)",
	"Pad 8 (sweep)", "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)",
	"FX 4 (atmosphere)", "FX 5 (brightness)", "FX 6 (goblins)", "FX 7 (echoes)",
	"FX 8 (sci-fi)", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe",
	"Fiddle", "Shanai", "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock",
	"Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal", "Guitar Fret Noise",
	"Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter",
	"Applause", "Gunshot"
};

static const char * const smpte_rates[] = {
	"24 fps",
	"25 fps",
	"30 fps (drop frame)",
	"30 fps (non drop frame)"
};

static const char * const major_keys[] = {
	"Unknown major", "Fb", "Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F",
	"C", "G", "D", "A", "E", "B", "F#", "C#", "G#", "Unknown major"
};

static const char * const minor_keys[] = {
	"Unknown minor", "Dbm", "Abm", "Ebm", "Bbm", "Fm", "Cm", "Gm", "Dm",
	"Am", "Em", "Bm", "F#m", "C#m", "G#m", "D#m", "A#m", "E#m", "Unknown minor"
};

static const char *controller_name(uint8_t ctrl)
{
	if (ctrl > 127 ||
	    controller_names[ctrl] == NULL)
		return "Unknown";
	return controller_names[ctrl];
}

static void dump_mem(FILE *out, const char *label, const uint8_t *data, uint32_t size)
{
	fprintf(out, "%s: ", label);
	while (size--)
		fprintf(out, "%02x ", *data++);
}

static int dump_event_midi1(FILE *out, const struct midi_event *ev)
{
	fprintf(out, "track:%2d sec:%f ", ev->track, ev->sec);

	switch (ev->data[0]) {
	case 0x80 ... 0x8f:
		fprintf(out, "Note Off   (channel %2d): note %3s%d, velocity %3d",
				(ev->data[0] & 0x0f) + 1,
				note_names[ev->data[1] % 12], ev->data[1] / 12 -1,
				ev->data[2]);
		break;
	case 0x90 ... 0x9f:
		fprintf(out, "Note On    (channel %2d): note %3s%d, velocity %3d",
				(ev->data[0] & 0x0f) + 1,
				note_names[ev->data[1] % 12], ev->data[1] / 12 -1,
				ev->data[2]);
		break;
	case 0xa0 ... 0xaf:
		fprintf(out, "Aftertouch (channel %2d): note %3s%d, pressure %3d",
				(ev->data[0] & 0x0f) + 1,
				note_names[ev->data[1] % 12], ev->data[1] / 12 -1,
				ev->data[2]);
		break;
	case 0xb0 ... 0xbf:
		fprintf(out, "Controller (channel %2d): controller %3d (%s), value %3d",
				(ev->data[0] & 0x0f) + 1, ev->data[1],
				controller_name(ev->data[1]), ev->data[2]);
		break;
	case 0xc0 ... 0xcf:
		fprintf(out, "Program    (channel %2d): program %3d (%s)",
				(ev->data[0] & 0x0f) + 1, ev->data[1],
				program_names[ev->data[1]]);
		break;
	case 0xd0 ... 0xdf:
		fprintf(out, "Channel Pressure (channel %2d): pressure %3d",
				(ev->data[0] & 0x0f) + 1, ev->data[1]);
		break;
	case 0xe0 ... 0xef:
		fprintf(out, "Pitch Bend (channel %2d): value %d", (ev->data[0] & 0x0f) + 1,
				((int)ev->data[2] << 7 | ev->data[1]) - 0x2000);
		break;
	case 0xf0:
	case 0xf7:
		dump_mem(out, "SysEx", ev->data, ev->size);
		break;
	case 0xf1:
		fprintf(out, "MIDI Time Code Quarter Frame: type %d values %d",
				ev->data[0] >> 4, ev->data[0] & 0xf);
		break;
	case 0xf2:
		fprintf(out, "Song Position Pointer: value %d",
				((int)ev->data[1] << 7 | ev->data[0]));
		break;
	case 0xf3:
		fprintf(out, "Song Select: value %d", (ev->data[0] & 0x7f));
		break;
	case 0xf6:
		fprintf(out, "Tune Request");
		break;
	case 0xf8:
		fprintf(out, "Timing Clock");
		break;
	case 0xfa:
		fprintf(out, "Start Sequence");
		break;
	case 0xfb:
		fprintf(out, "Continue Sequence");
		break;
	case 0xfc:
		fprintf(out, "Stop Sequence");
		break;
	case 0xfe:
		fprintf(out, "Active Sensing");
		break;
	case 0xff:
	{
		uint8_t *meta = &ev->data[ev->meta.offset];
		fprintf(out, "Meta: ");
		switch (ev->data[1]) {
		case 0x00:
			fprintf(out, "Sequence Number %3d %3d", meta[0], meta[1]);
			break;
		case 0x01 ... 0x09:
			fprintf(out, "%s: %s", event_names[ev->data[1] - 1], meta);
			break;
		case 0x20:
			fprintf(out, "Channel Prefix: %03d", meta[0]);
			break;
		case 0x21:
			fprintf(out, "Midi Port: %03d", meta[0]);
			break;
		case 0x2f:
			fprintf(out, "End Of Track");
			break;
		case 0x51:
			fprintf(out, "Tempo: %d microseconds per quarter note, %.2f BPM",
					ev->meta.parsed.tempo.uspqn,
					60000000.0 / (double)ev->meta.parsed.tempo.uspqn);
			break;
		case 0x54:
			fprintf(out, "SMPTE Offset: %s %02d:%02d:%02d:%02d.%03d",
					smpte_rates[(meta[0] & 0x60) >> 5],
					meta[0] & 0x1f, meta[1], meta[2],
					meta[3], meta[4]);
			break;
		case 0x58:
			fprintf(out, "Time Signature: %d/%d, %d clocks per click, %d notated 32nd notes per quarter note",
				meta[0], (int)pow(2, meta[1]), meta[2], meta[3]);
			break;
		case 0x59:
		{
			int sf = meta[0];
			fprintf(out, "Key Signature: %d %s: %s", abs(sf),
					sf > 0 ? "sharps" : "flats",
					meta[1] == 0 ?
						major_keys[SPA_CLAMP(sf + 9, 0, 18)] :
						minor_keys[SPA_CLAMP(sf + 9, 0, 18)]);
			break;
		}
		case 0x7f:
			dump_mem(out, "Sequencer", ev->data, ev->size);
			break;
		default:
			dump_mem(out, "Invalid", ev->data, ev->size);
		}
		break;
	}
	default:
		dump_mem(out, "Unknown", ev->data, ev->size);
		break;
	}
	return 0;
}

static int dump_event_midi2_channel(FILE *out, const struct midi_event *ev)
{
	uint32_t *d = (uint32_t*)ev->data;
	uint8_t status = d[0] >> 16;

	fprintf(out, "track:%2d sec:%f ", ev->track, ev->sec);

	switch (status) {
	case 0x00 ... 0x0f:
	case 0x10 ... 0x1f:
	{
		uint8_t note = (d[0] >> 8) & 0x7f;
		uint8_t index = d[0] & 0xff;
		fprintf(out, "%s Per-Note controller (channel %2d): note %3s%d, index %u, value %u",
				(status & 0xf0) == 0x00 ? "Registered" : "Assignable",
				(status & 0x0f) + 1,
				note_names[note % 12], note / 12 -1, index, d[1]);
		break;
	}
	case 0x20 ... 0x2f:
	case 0x30 ... 0x3f:
	{
		uint16_t index = (d[0] & 0x7f) | ((d[0] & 0x7f00) >> 1);
		fprintf(out, "%s controller (channel %2d): index %u, value %u",
				(status & 0xf0) == 0x20 ? "Registered" : "Assignable",
				(status & 0x0f) + 1, index, d[1]);
		break;
	}
	case 0x40 ... 0x4f:
	case 0x50 ... 0x5f:
	{
		uint16_t index = (d[0] & 0x7f) | ((d[0] & 0x7f00) >> 1);
		fprintf(out, "Relative %s controller (channel %2d): index %u, value %u",
				(status & 0xf0) == 0x20 ? "Registered" : "Assignable",
				(status & 0x0f) + 1, index, d[1]);
		break;
	}
	case 0x60 ... 0x6f:
	{
		uint8_t note = (d[0] >> 8) & 0x7f;
		fprintf(out, "Per-Note Pitch Bend  (channel %2d): note %3s%d, pitch %u",
				(status & 0x0f) + 1,
				note_names[note % 12], note / 12 -1, d[1]);
		break;
	}
	case 0x80 ... 0x8f:
	{
		uint8_t note = (d[0] >> 8) & 0x7f;
		uint8_t attr_type = d[0] & 0xff;
		uint16_t velocity = (d[1] >> 16) & 0xffff;
		uint16_t attr_data = (d[1]) & 0xffff;
		fprintf(out, "Note Off   (channel %2d): note %3s%d, velocity %5d, attr (%u)%u",
				(status & 0x0f) + 1,
				note_names[note % 12], note / 12 -1,
				velocity, attr_type, attr_data);
		break;
	}
	case 0x90 ... 0x9f:
	{
		uint8_t note = (d[0] >> 8) & 0x7f;
		uint8_t attr_type = d[0] & 0xff;
		uint16_t velocity = (d[1] >> 16) & 0xffff;
		uint16_t attr_data = (d[1]) & 0xffff;
		fprintf(out, "Note On    (channel %2d): note %3s%d, velocity %5d, attr (%u)%u",
				(status & 0x0f) + 1,
				note_names[note % 12], note / 12 -1,
				velocity, attr_type, attr_data);
		break;
	}
	case 0xa0 ... 0xaf:
	{
		uint8_t note = (d[0] >> 8) & 0x7f;
		fprintf(out, "Aftertouch (channel %2d): note %3s%d, pressure %u",
				(status & 0x0f) + 1,
				note_names[note % 12], note / 12 -1, d[1]);
		break;
	}
	case 0xb0 ... 0xbf:
	{
		uint8_t index = (d[0] >> 8) & 0x7f;
		fprintf(out, "Controller (channel %2d): controller %3d (%s), value %u",
				(status & 0x0f) + 1, index,
				controller_name(index), d[1]);
		break;
	}
	case 0xc0 ... 0xcf:
	{
		uint8_t flags = (d[0] & 0xff);
		uint8_t program = (d[1] >> 24) & 0x7f;
		uint16_t bank = (d[1] & 0x7f) | ((d[1] & 0x7f00) >> 1);
		fprintf(out, "Program    (channel %2d): flags %u program %3d (%s), bank %u",
				(status & 0x0f) + 1, flags, program,
				program_names[program], bank);
		break;
	}
	case 0xd0 ... 0xdf:
		fprintf(out, "Channel Pressure (channel %2d): pressure %u",
				(status & 0x0f) + 1, d[1]);
		break;
	case 0xe0 ... 0xef:
		fprintf(out, "Pitch Bend (channel %2d): value %u",
				(status & 0x0f) + 1, d[1]);
		break;
	case 0xf0 ... 0xff:
	{
		uint8_t note = (d[0] >> 8) & 0x7f;
		uint8_t flags = d[0] & 0xff;
		fprintf(out, "Per-Note management (channel %2d): note %3s%d, flags %u",
				(status & 0x0f) + 1,
				note_names[note % 12], note / 12 -1, flags);
		break;
	}
	default:
		dump_mem(out, "Unknown", ev->data, ev->size);
		break;
	}

	return 0;
}

static int dump_event_ump(FILE *out, const struct midi_event *ev)
{
	uint32_t *d = (uint32_t*)ev->data;
	uint8_t group = (d[0] >> 24) & 0xf;
	uint8_t mt = (d[0] >> 28) & 0xf;
	int res = 0;

	fprintf(out, "group:%2d ", group);

	switch (mt) {
	case 0x0:
		switch ((d[0] >> 20) & 0xf) {
		case 0x1:
			fprintf(out, "JR clock: value %d", d[0] & 0xffff);
			break;
		case 0x2:
			fprintf(out, "JR timestamp: value %d", d[0] & 0xffff);
			break;
		case 0x3:
			fprintf(out, "DCTPQ: value %d", d[0] & 0xffff);
			break;
		case 0x4:
			fprintf(out, "DC: value %d", d[0] & 0xfffff);
			break;
		default:
			dump_mem(out, "Utility unkown", ev->data, ev->size);
		}
		break;
	case 0x1:
	{
		uint8_t b[3] = { (d[0] >> 16) & 0x7f, (d[0] >> 8) & 0x7f, d[0] & 0x7f };
		switch (b[0]) {
		case 0xf1:
			fprintf(out, "MIDI Time Code Quarter Frame: type %d values %d",
					b[1] >> 4, b[1] & 0xf);
			break;
		case 0xf2:
			fprintf(out, "Song Position Pointer: value %d",
					((int)b[2] << 7 | b[1]));
			break;
		case 0xf3:
			fprintf(out, "Song Select: value %d", b[1]);
			break;
		case 0xf6:
			fprintf(out, "Tune Request");
			break;
		case 0xf8:
			fprintf(out, "Timing Clock");
			break;
		case 0xfa:
			fprintf(out, "Start Sequence");
			break;
		case 0xfb:
			fprintf(out, "Continue Sequence");
			break;
		case 0xfc:
			fprintf(out, "Stop Sequence");
			break;
		case 0xfe:
			fprintf(out, "Active Sensing");
			break;
		case 0xff:
			fprintf(out, "System Reset");
			break;
		default:
			dump_mem(out, "SysRT", ev->data, ev->size);
			break;
		}
		break;
	}
	case 0x2:
	{
		struct midi_event ev1;
		uint8_t b[3] = { d[0] >> 16, d[0] >> 8, d[0] };

		ev1 = *ev;
		if (b[0] >= 0xc0 && b[0] <= 0xdf)
			ev1.size = 2;
                else
			ev1.size = 3;
		ev1.data = b;
		dump_event_midi1(out, &ev1);
		break;
	}
	case 0x3:
	{
		uint8_t status = (d[0] >> 20) & 0xf;
		uint8_t bytes = SPA_CLAMP((d[0] >> 16) & 0xf, 0u, 6u);
		uint8_t b[6] = { d[0] >> 8, d[0], d[1] >> 24, d[1] >> 16, d[1] >> 8, d[1] };
		switch (status) {
		case 0x0:
			dump_mem(out, "SysEx7 (Complete) ", b, bytes);
			break;
		case 0x1:
			dump_mem(out, "SysEx7 (Start)    ", b, bytes);
			break;
		case 0x2:
			dump_mem(out, "SysEx7 (Continue) ", b, bytes);
			break;
		case 0x3:
			dump_mem(out, "SysEx7 (End)      ", b, bytes);
			break;
		default:
			dump_mem(out, "SysEx7 (invalid)", ev->data, ev->size);
			break;
		}
		break;
	}
	case 0x4:
		res = dump_event_midi2_channel(out, ev);
		break;
	case 0x5:
		dump_mem(out, "Data128", ev->data, ev->size);
		break;
	default:
		dump_mem(out, "Reserved", ev->data, ev->size);
		break;
	}
	return res;
}

int midi_event_dump(FILE *out, const struct midi_event *ev)
{
	int res;
	switch (ev->type) {
	case MIDI_EVENT_TYPE_MIDI1:
		res = dump_event_midi1(out, ev);
		break;
	case MIDI_EVENT_TYPE_UMP:
		res = dump_event_ump(out, ev);
		break;
	default:
		return -EINVAL;
	}
	fprintf(out, "\n");
	return res;
}
