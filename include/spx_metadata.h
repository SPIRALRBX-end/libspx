#ifndef SPX_METADATA_H
#define SPX_METADATA_H

#include <stdio.h>
#include <stdint.h>

#define ID3V1_TAG_SIZE 128

typedef struct {
    char   title[31];
    char   artist[31];
    char   album[31];
    char   year[5];
    char   comment[31];
    uint8_t genre;
    char   track[6];
    char   album_artist[31];
    uint8_t rating;
} metadata_t;

int  read_id3v1(const char *fn, metadata_t *m);

void calc_audio_props(const char *fn,
                      int sample_rate,
                      int total_samples,
                      double *out_secs,
                      uint32_t *out_bitrate);

void write_meta_chunk(FILE *out,
                      const metadata_t *m,
                      double length_s,
                      uint32_t bitrate,
                      const char *infile);

#endif