#include "spx_encoder.h"

spx_encoder_t* spx_encoder_create(long sample_rate, int channels) {
    spx_encoder_t *e = malloc(sizeof(*e));
    e->sample_rate  = sample_rate;
    e->channels     = channels;
    e->last_samples = calloc(channels, sizeof(int16_t));
    return e;
}

int spx_encode_frame(spx_encoder_t *enc,
                     const int16_t *pcm,
                     int frame_size,
                     uint8_t *out_buf)
{
    int samples = frame_size * enc->channels;
    int16_t *delta = (int16_t*)out_buf;
    for (int i = 0; i < samples; i++) {
        int c = i % enc->channels;
        int32_t d = (int32_t)pcm[i] - enc->last_samples[c];
        int16_t dv = (d>32767?32767:(d<-32768?-32768:(int16_t)d));
        delta[i] = dv;
        enc->last_samples[c] += dv;
    }
    return samples * sizeof(int16_t);
}

int spx_encoder_finish(spx_encoder_t *enc, uint8_t *out_buf) {
    (void)enc; (void)out_buf;
    return 0;
}

void spx_encoder_destroy(spx_encoder_t *enc) {
    if (!enc) return;
    free(enc->last_samples);
    free(enc);
}