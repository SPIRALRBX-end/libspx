#ifndef SPX_ENCODER_H
#define SPX_ENCODER_H

#include <stdlib.h>
#include <stdint.h>

typedef struct {
    long   sample_rate;
    int    channels;
    int16_t *last_samples;
} spx_encoder_t;

spx_encoder_t* spx_encoder_create(long sample_rate, int channels);
int            spx_encode_frame(spx_encoder_t *enc,
                                 const int16_t *pcm,
                                 int frame_size,
                                 uint8_t *out_buf);
int            spx_encoder_finish(spx_encoder_t *enc, uint8_t *out_buf);
void           spx_encoder_destroy(spx_encoder_t *enc);

#endif
