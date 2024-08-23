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

#include "midifile.h"

#define DEFAULT_TEMPO	500000	/* 500ms per quarter note (120 BPM) is the default */

struct midi_track {
	uint16_t id;

	uint8_t *data;
	uint32_t size;

	uint8_t *p;
	int64_t tick;
	unsigned int eof:1;
	uint8_t event[4];
};

struct midi_file {
	uint8_t *data;
	size_t size;

	int mode;
	int fd;

	struct midi_file_info info;
	uint32_t length;
	uint32_t tempo;

	uint8_t *p;
	int64_t tick;
	double tick_sec;
	double tick_start;

	struct midi_track tracks[64];
};

static inline uint16_t parse_be16(const uint8_t *in)
{
	return (in[0] << 8) | in[1];
}

static inline uint32_t parse_be32(const uint8_t *in)
{
	return (in[0] << 24) | (in[1] << 16) | (in[2] << 8) | in[3];
}

static inline int mf_avail(struct midi_file *mf)
{
	if (mf->p < mf->data + mf->size)
		return mf->size + mf->data - mf->p;
	return 0;
}

static inline int tr_avail(struct midi_track *tr)
{
	if (tr->eof)
		return 0;
	if (tr->p < tr->data + tr->size)
		return tr->size + tr->data - tr->p;
	tr->eof = true;
	return 0;
}

static int read_mthd(struct midi_file *mf)
{
	if (mf_avail(mf) < 14 ||
	    memcmp(mf->p, "MThd", 4) != 0)
		return -EINVAL;

	mf->length = parse_be32(mf->p + 4);
	mf->info.format = parse_be16(mf->p + 8);
	mf->info.ntracks = parse_be16(mf->p + 10);
	mf->info.division = parse_be16(mf->p + 12);

	mf->p += 14;
	return 0;
}

static int read_mtrk(struct midi_file *mf, struct midi_track *track)
{
	if (mf_avail(mf) < 8 ||
	    memcmp(mf->p, "MTrk", 4) != 0)
		return -EINVAL;

	track->data = track->p = mf->p + 8;
	track->size = parse_be32(mf->p + 4);

	mf->p = track->data + track->size;
	if (mf->p > mf->data + mf->size)
		return -EINVAL;

	return 0;
}

static int parse_varlen(struct midi_file *mf, struct midi_track *tr, uint32_t *result)
{
	uint32_t value = 0;

	while (tr_avail(tr) > 0) {
		uint8_t b = *tr->p++;
		value = (value << 7) | (b & 0x7f);
		if ((b & 0x80) == 0)
			break;
	}
	*result = value;
	return 0;
}

static int open_read(struct midi_file *mf, const char *filename, struct midi_file_info *info)
{
	int res;
	uint16_t i;
	struct stat st;

	if ((mf->fd = open(filename, O_RDONLY)) < 0) {
		res = -errno;
		goto exit;
	}
	if (fstat(mf->fd, &st) < 0) {
		res = -errno;
		goto exit_close;
	}
	mf->size = st.st_size;

	mf->data = mmap(NULL, mf->size, PROT_READ, MAP_SHARED, mf->fd, 0);
	if (mf->data == MAP_FAILED) {
		res = -errno;
		goto exit_close;
	}

	mf->p = mf->data;

	if ((res = read_mthd(mf)) < 0)
		goto exit_unmap;

	mf->tempo = DEFAULT_TEMPO;
	mf->tick = 0;

	for (i = 0; i < mf->info.ntracks; i++) {
		struct midi_track *tr = &mf->tracks[i];
		uint32_t delta_time;

		if ((res = read_mtrk(mf, tr)) < 0)
			goto exit_unmap;

		if ((res = parse_varlen(mf, tr, &delta_time)) < 0)
			goto exit_unmap;

		tr->tick = delta_time;
		tr->id = i;
	}
	mf->mode = 1;
	*info = mf->info;
	return 0;

exit_unmap:
	munmap(mf->data, mf->size);
exit_close:
	close(mf->fd);
exit:
	return res;
}

static inline int write_n(int fd, const void *buf, int count)
{
	return write(fd, buf, count) == (ssize_t)count ? count : -errno;
}

static inline int write_be16(int fd, uint16_t val)
{
	uint8_t buf[2] = { val >> 8, val };
	return write_n(fd, buf, 2);
}

static inline int write_be32(int fd, uint32_t val)
{
	uint8_t buf[4] = { val >> 24, val >> 16, val >> 8, val };
	return write_n(fd, buf, 4);
}

#define CHECK_RES(expr) if ((res = (expr)) < 0) return res

static int write_headers(struct midi_file *mf)
{
	struct midi_track *tr = &mf->tracks[0];
	int res;

	lseek(mf->fd, 0, SEEK_SET);

	mf->length = 6;
	CHECK_RES(write_n(mf->fd, "MThd", 4));
	CHECK_RES(write_be32(mf->fd, mf->length));
	CHECK_RES(write_be16(mf->fd, mf->info.format));
	CHECK_RES(write_be16(mf->fd, mf->info.ntracks));
	CHECK_RES(write_be16(mf->fd, mf->info.division));

	CHECK_RES(write_n(mf->fd, "MTrk", 4));
	CHECK_RES(write_be32(mf->fd, tr->size));

	return 0;
}

static int open_write(struct midi_file *mf, const char *filename, struct midi_file_info *info)
{
	int res;

	if (info->format != 0)
		return -EINVAL;
	if (info->ntracks == 0)
		info->ntracks = 1;
	else if (info->ntracks != 1)
		return -EINVAL;
	if (info->division == 0)
		info->division = 96;

	if ((mf->fd = open(filename, O_WRONLY | O_CREAT, 0660)) < 0) {
		res = -errno;
		goto exit;
	}
	mf->mode = 2;
	mf->tempo = DEFAULT_TEMPO;
	mf->info = *info;

	res = write_headers(mf);
exit:
	return res;
}

struct midi_file *
midi_file_open(const char *filename, const char *mode, struct midi_file_info *info)
{
	int res;
	struct midi_file *mf;

	mf = calloc(1, sizeof(struct midi_file));
	if (mf == NULL)
		return NULL;

	if (spa_streq(mode, "r")) {
		if ((res = open_read(mf, filename, info)) < 0)
			goto exit_free;
	} else if (spa_streq(mode, "w")) {
		if ((res = open_write(mf, filename, info)) < 0)
			goto exit_free;
	} else {
		res = -EINVAL;
		goto exit_free;
	}
	return mf;

exit_free:
	free(mf);
	errno = -res;
	return NULL;
}

int midi_file_close(struct midi_file *mf)
{
	int res;

	if (mf->mode == 1) {
		munmap(mf->data, mf->size);
	} else if (mf->mode == 2) {
		uint8_t buf[4] = { 0x00, 0xff, 0x2f, 0x00 };
		CHECK_RES(write_n(mf->fd, buf, 4));
		mf->tracks[0].size += 4;
		CHECK_RES(write_headers(mf));
	} else
		return -EINVAL;

	close(mf->fd);
	free(mf);
	return 0;
}

static int peek_next(struct midi_file *mf, struct midi_event *ev)
{
	struct midi_track *tr, *found = NULL;
	uint16_t i;

	for (i = 0; i < mf->info.ntracks; i++) {
		tr = &mf->tracks[i];
		if (tr_avail(tr) == 0)
			continue;
		if (found == NULL || tr->tick < found->tick)
			found = tr;
	}
	if (found == NULL)
		return 0;

	ev->track = found->id;
	ev->sec = mf->tick_sec + ((found->tick - mf->tick_start) * (double)mf->tempo) / (1000000.0 * mf->info.division);
	ev->type = MIDI_EVENT_TYPE_MIDI1;
	return 1;
}

int midi_file_next_time(struct midi_file *mf, double *sec)
{
	struct midi_event ev;
	int res;

	if ((res = peek_next(mf, &ev)) <= 0)
		return res;

	*sec = ev.sec;
	return 1;
}

int midi_file_read_event(struct midi_file *mf, struct midi_event *event)
{
	struct midi_track *tr;
	uint32_t delta_time, size;
	uint8_t status, meta;
	int res, running;

	if ((res = peek_next(mf, event)) <= 0)
		return res;

	tr = &mf->tracks[event->track];
	status = *tr->p;

	running = (status & 0x80) == 0;
	if (running) {
		status = tr->event[0];
		event->data = tr->event;
	} else {
		event->data = tr->p++;
		tr->event[0] = status;
	}

	switch (status) {
	case 0xc0 ... 0xdf:
		size = 2;
		break;

	case 0x80 ... 0xbf:
	case 0xe0 ... 0xef:
		size = 3;
		break;

	case 0xff:
		meta = *tr->p++;

		if ((res = parse_varlen(mf, tr, &size)) < 0)
			return res;

		event->meta.offset = tr->p - event->data;
		event->meta.size = size;

		switch (meta) {
		case 0x2f:
			tr->eof = true;
			break;
		case 0x51:
			if (size < 3)
				return -EINVAL;
			mf->tick_sec = event->sec;
			mf->tick_start = tr->tick;
			event->meta.parsed.tempo.uspqn = mf->tempo = (tr->p[0]<<16) | (tr->p[1]<<8) | tr->p[2];
			break;
		}
		size += tr->p - event->data;
		break;

	case 0xf0:
	case 0xf7:
		if ((res = parse_varlen(mf, tr, &size)) < 0)
			return res;
		size += tr->p - event->data;
		break;
	default:
		return -EINVAL;
	}

	event->size = size;

	if (running) {
		memcpy(&event->data[1], tr->p, size - 1);
		tr->p += size - 1;
	} else {
		tr->p = event->data + event->size;
	}

	if ((res = parse_varlen(mf, tr, &delta_time)) < 0)
		return res;

	tr->tick += delta_time;
	return 1;
}

static int write_varlen(struct midi_file *mf, struct midi_track *tr, uint32_t value)
{
	uint64_t buffer;
	uint8_t b;
	int res;

	buffer = value & 0x7f;
	while ((value >>= 7)) {
		buffer <<= 8;
		buffer |= ((value & 0x7f) | 0x80);
	}
        do  {
		b = buffer & 0xff;
		CHECK_RES(write_n(mf->fd, &b, 1));
		tr->size++;
		buffer >>= 8;
	} while (b & 0x80);

	return 0;
}

int midi_file_write_event(struct midi_file *mf, const struct midi_event *event)
{
	struct midi_track *tr;
	uint32_t tick;
	void *data;
	size_t size;
	int res;
	uint8_t ev[32];

	spa_return_val_if_fail(event != NULL, -EINVAL);
	spa_return_val_if_fail(mf != NULL, -EINVAL);
	spa_return_val_if_fail(event->track == 0, -EINVAL);
	spa_return_val_if_fail(event->size > 1, -EINVAL);
	spa_return_val_if_fail(event->type == MIDI_EVENT_TYPE_MIDI1, -EINVAL);

	switch (event->type) {
	case MIDI_EVENT_TYPE_MIDI1:
		data = event->data;
		size = event->size;
		break;
	case MIDI_EVENT_TYPE_UMP:
		data = ev;
		size = spa_ump_to_midi((uint32_t*)event->data, event->size, ev, sizeof(ev));
		if (size == 0)
			return 0;
		break;
	default:
		return -EINVAL;
	}

	tr = &mf->tracks[event->track];

	tick = (uint32_t)(event->sec * (1000000.0 * mf->info.division) / (double)mf->tempo);

	CHECK_RES(write_varlen(mf, tr, tick - tr->tick));
	tr->tick = tick;

	CHECK_RES(write_n(mf->fd, data, size));
	tr->size += size;

	return 0;
}

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

static void dump_mem(FILE *out, const char *label, uint8_t *data, uint32_t size)
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
		dump_mem(out, "Utility", ev->data, ev->size);
		break;
	case 0x1:
		dump_mem(out, "SysRT", ev->data, ev->size);
		break;
	case 0x2:
	{
		struct midi_event ev1;
		uint8_t msg[4];

		ev1 = *ev;
		msg[0] = (d[0] >> 16);
		msg[1] = (d[0] >> 8);
		msg[2] = (d[0]);
		if (msg[0] >= 0xc0 && msg[0] <= 0xdf)
			ev1.size = 2;
                else
			ev1.size = 3;
		ev1.data = msg;
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

int midi_file_dump_event(FILE *out, const struct midi_event *ev)
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
