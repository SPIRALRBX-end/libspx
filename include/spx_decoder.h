#ifndef SPX_DECODER_H
#define SPX_DECODER_H

#include <stdio.h>
#include <stdint.h>

#ifdef USE_MPG123
#  include <mpg123.h>
#endif
#ifdef USE_VORBIS
#  include <vorbis/vorbisfile.h>
#endif

typedef enum {
    FORMAT_UNKNOWN = 0,
    FORMAT_WAV,
    FORMAT_MP3,
    FORMAT_OGG,
    FORMAT_SPX
} audio_format_t;

typedef struct {
    audio_format_t format;
    FILE         * file;
    int             sample_rate;
    int             channels;
    int             total_samples;
    int             current_sample;
    int16_t       * buffer;
    int             buffer_size;
#ifdef USE_MPG123
    mpg123_handle * mp3_handle;
#endif
#ifdef USE_VORBIS
    OggVorbis_File  vorbis_file;
#endif
} audio_decoder_t;

audio_format_t   detect_format(const char *fn);
audio_decoder_t* create_decoder(const char *fn);
int              read_samples(audio_decoder_t *d, int16_t *buf, int max_samps);
void             destroy_decoder(audio_decoder_t *d);

#endif
