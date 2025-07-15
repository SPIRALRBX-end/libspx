#ifndef SPX_CODEC_H
#define SPX_CODEC_H

#include <stdio.h>
#include <stdint.h>

void write_spx_header(FILE *f, int sr, int ch);
int  read_spx_header(FILE *f, int *sr, int *ch);

void write_wav_header(FILE *f, int sr, int ch, uint32_t data_sz);
int  audio_to_spx(const char *infile, const char *outfile);
int  spx_to_wav(const char *spxfile, const char *wavfile);
int  spx_to_mp3(const char *spxfile, const char *mp3file);
int  spx_to_ogg(const char *spxfile, const char *oggfile);

#endif