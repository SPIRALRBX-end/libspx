#include "spx_decoder.h"
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 8192

audio_format_t detect_format(const char *fn) {
    const char *ext = strrchr(fn, '.');
    if (!ext) return FORMAT_UNKNOWN;
    if (strcasecmp(ext, ".wav")==0) return FORMAT_WAV;
    if (strcasecmp(ext, ".mp3")==0) return FORMAT_MP3;
    if (strcasecmp(ext, ".ogg")==0) return FORMAT_OGG;
    if (strcasecmp(ext, ".spx")==0) return FORMAT_SPX;
    return FORMAT_UNKNOWN;
}

audio_decoder_t* create_decoder(const char *fn) {
    audio_decoder_t *d = calloc(1, sizeof(*d));
    d->format = detect_format(fn);
    d->buffer = malloc(BUFFER_SIZE * sizeof(int16_t));
    d->buffer_size = BUFFER_SIZE;

    switch (d->format) {
    case FORMAT_WAV:
        break;
#ifdef USE_MPG123
    case FORMAT_MP3:
        mpg123_init();
        d->mp3_handle = mpg123_new(NULL, NULL);
        if (mpg123_open(d->mp3_handle, fn) != MPG123_OK) break;
        {
            long rate; int ch, enc;
            mpg123_getformat(d->mp3_handle, &rate, &ch, &enc);
            mpg123_format_none(d->mp3_handle);
            mpg123_format(d->mp3_handle, rate, ch, MPG123_ENC_SIGNED_16);
            d->sample_rate = rate;
            d->channels    = ch;
        }
        return d;
#endif
#ifdef USE_VORBIS
    case FORMAT_OGG:
        if (ov_open(fopen(fn,"rb"), &d->vorbis_file, NULL, 0) != 0) break;
        {
            vorbis_info *vi = ov_info(&d->vorbis_file, -1);
            d->sample_rate   = vi->rate;
            d->channels      = vi->channels;
            d->total_samples = ov_pcm_total(&d->vorbis_file, -1);
        }
        return d;
#endif
    default:
        break;
    }

    destroy_decoder(d);
    return NULL;
}

int read_samples(audio_decoder_t *d, int16_t *buf, int max_samps) {
    if (!d) return 0;
    switch (d->format) {
    case FORMAT_WAV:
        return 0;
#ifdef USE_MPG123
    case FORMAT_MP3: {
        size_t done = 0;
        mpg123_read(d->mp3_handle, (unsigned char*)buf,
                    max_samps * sizeof(int16_t), &done);
        return done / sizeof(int16_t);
    }
#endif
#ifdef USE_VORBIS
    case FORMAT_OGG: {
        int read=0, sec;
        while (read < max_samps) {
            long ret = ov_read(&d->vorbis_file,
                               (char*)(buf+read),
                               (max_samps-read)*sizeof(int16_t),
                               0,2,1, &sec);
            if (ret <= 0) break;
            read += ret/sizeof(int16_t);
        }
        return read;
    }
#endif
    default:
        return 0;
    }
}

void destroy_decoder(audio_decoder_t *d) {
    if (!d) return;
    if (d->file) fclose(d->file);
#ifdef USE_MPG123
    if (d->mp3_handle) {
        mpg123_close(d->mp3_handle);
        mpg123_delete(d->mp3_handle);
        mpg123_exit();
    }
#endif
#ifdef USE_VORBIS
    if (d->format == FORMAT_OGG) ov_clear(&d->vorbis_file);
#endif
    free(d->buffer);
    free(d);
}
