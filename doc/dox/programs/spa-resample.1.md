\page page_man_spa-resample_1 spa-resample

The PipeWire resampler debugging utility

# SYNOPSIS

**spa-resample** \[*OPTIONS*\] *INFILE* *OUTFILE*

# DESCRIPTION

Use the PipeWire resampler to resample input file to output file,
following the given options.

This is useful only for testing the resampler.

# OPTIONS

\par -r RATE | \--rate=RATE
Output sample rate.

\par -f FORMAT | \--format=FORMAT
Output sample format (s8 | s16 | s32 | f32 | f64).

\par -q QUALITY | \--quality=QUALITY
Resampler output quality (0-14).

\par -c FLAGS | \--cpuflags=FLAGS
See \ref spa_cpu "spa/support/cpu.h".

\par -h
Show help.

\par -v
Verbose operation.

# EXAMPLES

**spa-resample** -r 48000 -f s32 in.wav out.wav

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)"
