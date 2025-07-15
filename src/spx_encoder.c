#include "spx_encoder.h"
#include <string.h>

// Tabela de quantização adaptativa
static const int16_t quant_table[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28,
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448,
    512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096
};

#define QUANT_LEVELS (sizeof(quant_table)/sizeof(quant_table[0]))

// Compressão RLE simples
static int compress_rle(const uint8_t *in, int in_len, uint8_t *out) {
    int out_pos = 0;
    for (int i = 0; i < in_len; ) {
        uint8_t val = in[i];
        int count = 1;
        
        while (i + count < in_len && in[i + count] == val && count < 127) {
            count++;
        }
        
        if (count > 3 || val == 0) {
            out[out_pos++] = 0x80 | count;
            out[out_pos++] = val;
        } else {
            for (int j = 0; j < count; j++) {
                out[out_pos++] = val;
            }
        }
        i += count;
    }
    return out_pos;
}

// Quantização adaptativa
static int quantize_delta(int32_t delta) {
    int abs_delta = abs(delta);
    int sign = delta < 0 ? -1 : 1;
    
    for (int i = 0; i < QUANT_LEVELS - 1; i++) {
        if (abs_delta <= quant_table[i]) {
            return sign * i;
        }
    }
    return sign * (QUANT_LEVELS - 1);
}

static int32_t dequantize_delta(int quantized) {
    int sign = quantized < 0 ? -1 : 1;
    int idx = abs(quantized);
    if (idx >= QUANT_LEVELS) idx = QUANT_LEVELS - 1;
    return sign * quant_table[idx];
}

// Predição linear simples
static int16_t predict_sample(const int16_t *history, int channels, int channel) {
    if (!history) return 0;
    
    // Predição baseada em 2 amostras anteriores
    int16_t pred = history[channel] + (history[channel] - history[channel + channels]) / 2;
    return pred;
}

spx_encoder_t* spx_encoder_create(long sample_rate, int channels) {
    spx_encoder_t *e = malloc(sizeof(*e));
    e->sample_rate = sample_rate;
    e->channels = channels;
    e->last_samples = calloc(channels * 3, sizeof(int16_t)); // 3 amostras de história
    return e;
}

int spx_encode_frame(spx_encoder_t *enc, const int16_t *pcm, int frame_size, uint8_t *out_buf) {
    int samples = frame_size * enc->channels;
    uint8_t temp_buf[samples * 2]; // Buffer temporário
    int temp_pos = 0;
    
    // Codificação com predição e quantização
    for (int i = 0; i < samples; i++) {
        int c = i % enc->channels;
        
        // Predição linear
        int16_t pred = predict_sample(enc->last_samples, enc->channels, c);
        
        // Calcula o erro de predição
        int32_t error = (int32_t)pcm[i] - pred;
        
        // Quantização adaptativa
        int quantized = quantize_delta(error);
        
        // Armazena o valor quantizado (4 bits por amostra)
        if (i % 2 == 0) {
            temp_buf[temp_pos] = (quantized & 0x0F) << 4;
        } else {
            temp_buf[temp_pos] |= (quantized & 0x0F);
            temp_pos++;
        }
        
        // Atualiza histórico para predição
        memmove(&enc->last_samples[c * 3], &enc->last_samples[c * 3 + 1], 2 * sizeof(int16_t));
        
        // Reconstrói o valor para o histórico
        int32_t reconstructed = pred + dequantize_delta(quantized);
        if (reconstructed > 32767) reconstructed = 32767;
        if (reconstructed < -32768) reconstructed = -32768;
        enc->last_samples[c * 3 + 2] = (int16_t)reconstructed;
    }
    
    // Se número ímpar de amostras, completa o último byte
    if (samples % 2 == 1) {
        temp_pos++;
    }
    
    // Aplica compressão RLE
    int compressed_size = compress_rle(temp_buf, temp_pos, out_buf + 2);
    
    // Cabeçalho do frame (2 bytes): tamanho comprimido
    out_buf[0] = compressed_size & 0xFF;
    out_buf[1] = (compressed_size >> 8) & 0xFF;
    
    return compressed_size + 2;
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