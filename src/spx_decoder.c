#include "spx_decoder.h"
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 8192

// Tabela de quantização (mesma do encoder)
static const int16_t quant_table[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28,
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448,
    512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096
};

#define QUANT_LEVELS (sizeof(quant_table)/sizeof(quant_table[0]))

// Decodificação RLE
static int decompress_rle(const uint8_t *in, int in_len, uint8_t *out) {
    int out_pos = 0;
    for (int i = 0; i < in_len; ) {
        uint8_t val = in[i++];
        
        if (val & 0x80) {
            int count = val & 0x7F;
            if (i >= in_len) break;
            uint8_t data = in[i++];
            for (int j = 0; j < count; j++) {
                out[out_pos++] = data;
            }
        } else {
            out[out_pos++] = val;
        }
    }
    return out_pos;
}

// Dequantização
static int32_t dequantize_delta(int quantized) {
    int sign = quantized < 0 ? -1 : 1;
    int idx = abs(quantized);
    if (idx >= QUANT_LEVELS) idx = QUANT_LEVELS - 1;
    return sign * quant_table[idx];
}

// Predição linear
static int16_t predict_sample(const int16_t *history, int channels, int channel) {
    if (!history) return 0;
    return history[channel] + (history[channel] - history[channel + channels]) / 2;
}

// Clamp para 16 bits
static inline int16_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// Estrutura para decoder SPX
typedef struct {
    int16_t *history;
    int channels;
    uint8_t *compressed_buffer;
    uint8_t *temp_buffer;
} spx_decoder_data_t;

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
    
    switch (d->format) {
    case FORMAT_WAV: {
        d->file = fopen(fn, "rb");
        if (!d->file) break;
        
        // Lê header WAV
        typedef struct {
            char riff[4];
            uint32_t overall_size;
            char wave[4];
            char fmt_chunk[4];
            uint32_t fmt_len;
            uint16_t audio_fmt;
            uint16_t num_ch;
            uint32_t sample_rate;
            uint32_t byte_rate;
            uint16_t block_align;
            uint16_t bits_per_samp;
            char data_chunk[4];
            uint32_t data_size;
        } wav_hdr_t;
        
        wav_hdr_t hdr;
        if (fread(&hdr, sizeof(hdr), 1, d->file) != 1) break;
        
        if (memcmp(hdr.riff, "RIFF", 4) != 0 || 
            memcmp(hdr.wave, "WAVE", 4) != 0 ||
            memcmp(hdr.fmt_chunk, "fmt ", 4) != 0 ||
            memcmp(hdr.data_chunk, "data", 4) != 0) break;
            
        d->sample_rate = hdr.sample_rate;
        d->channels = hdr.num_ch;
        d->total_samples = hdr.data_size / (hdr.bits_per_samp / 8) / hdr.num_ch;
        d->current_sample = 0;
        
        d->buffer = malloc(BUFFER_SIZE * sizeof(int16_t));
        d->buffer_size = BUFFER_SIZE;
        
        return d;
    }
        
    case FORMAT_SPX: {
        d->file = fopen(fn, "rb");
        if (!d->file) break;
        
        // Lê cabeçalho SPX
        char magic[4];
        if (fread(magic, 4, 1, d->file) != 1 || memcmp(magic, "SPX3", 4) != 0) break;
        
        fread(&d->sample_rate, sizeof(d->sample_rate), 1, d->file);
        fread(&d->channels, sizeof(d->channels), 1, d->file);
        
        uint32_t bps;
        fread(&bps, sizeof(bps), 1, d->file);
        if (bps != 16) break;
        
        // Pula chunk de metadados se existir
        char tag[4];
        if (fread(tag, 1, 4, d->file) == 4) {
            if (memcmp(tag, "META", 4) == 0) {
                uint32_t msz;
                fread(&msz, 4, 1, d->file);
                fseek(d->file, msz, SEEK_CUR);
            } else {
                fseek(d->file, -4, SEEK_CUR);
            }
        }
        
        // Inicializa dados do decoder SPX
        spx_decoder_data_t *spx_data = malloc(sizeof(spx_decoder_data_t));
        spx_data->history = calloc(d->channels * 3, sizeof(int16_t));
        spx_data->channels = d->channels;
        spx_data->compressed_buffer = malloc(8192);
        spx_data->temp_buffer = malloc(8192);
        
        // Armazena ponteiro para dados específicos do SPX
        d->buffer = (int16_t*)spx_data; // Reutiliza o campo buffer
        d->buffer_size = 0;
        
        return d;
    }
    
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
            d->channels = ch;
        }
        d->buffer = malloc(BUFFER_SIZE * sizeof(int16_t));
        d->buffer_size = BUFFER_SIZE;
        return d;
#endif
        
#ifdef USE_VORBIS
    case FORMAT_OGG:
        if (ov_open(fopen(fn,"rb"), &d->vorbis_file, NULL, 0) != 0) break;
        {
            vorbis_info *vi = ov_info(&d->vorbis_file, -1);
            d->sample_rate = vi->rate;
            d->channels = vi->channels;
            d->total_samples = ov_pcm_total(&d->vorbis_file, -1);
        }
        d->buffer = malloc(BUFFER_SIZE * sizeof(int16_t));
        d->buffer_size = BUFFER_SIZE;
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
    case FORMAT_WAV: {
        int to_read = max_samps;
        if (d->current_sample + to_read > d->total_samples) {
            to_read = d->total_samples - d->current_sample;
        }
        if (to_read <= 0) return 0;
        
        int read = fread(buf, sizeof(int16_t), to_read, d->file);
        d->current_sample += read;
        return read;
    }
        
    case FORMAT_SPX: {
        spx_decoder_data_t *spx_data = (spx_decoder_data_t*)d->buffer;
        
        // Lê cabeçalho do frame (2 bytes)
        uint8_t frame_header[2];
        if (fread(frame_header, 1, 2, d->file) != 2) return 0;
        
        int compressed_size = frame_header[0] | (frame_header[1] << 8);
        if (compressed_size == 0 || compressed_size > 8192) return 0;
        
        // Lê dados comprimidos
        if (fread(spx_data->compressed_buffer, 1, compressed_size, d->file) != compressed_size) {
            return 0;
        }
        
        // Descomprime RLE
        int decompressed_size = decompress_rle(spx_data->compressed_buffer, compressed_size, spx_data->temp_buffer);
        
        // Decodifica amostras
        int samples_decoded = 0;
        for (int i = 0; i < decompressed_size && samples_decoded < max_samps; i++) {
            // Extrai 2 valores de 4 bits por byte
            for (int j = 0; j < 2 && samples_decoded < max_samps; j++) {
                int quantized = (j == 0) ? (spx_data->temp_buffer[i] >> 4) : (spx_data->temp_buffer[i] & 0x0F);
                
                // Converte para signed
                if (quantized > 7) quantized -= 16;
                
                int c = samples_decoded % d->channels;
                
                // Predição
                int16_t pred = predict_sample(spx_data->history, d->channels, c);
                
                // Reconstrói amostra
                int32_t reconstructed = pred + dequantize_delta(quantized);
                buf[samples_decoded] = clamp16(reconstructed);
                
                // Atualiza histórico
                memmove(&spx_data->history[c * 3], &spx_data->history[c * 3 + 1], 2 * sizeof(int16_t));
                spx_data->history[c * 3 + 2] = buf[samples_decoded];
                
                samples_decoded++;
            }
        }
        
        return samples_decoded;
    }
    
#ifdef USE_MPG123
    case FORMAT_MP3: {
        size_t done = 0;
        mpg123_read(d->mp3_handle, (unsigned char*)buf, max_samps * sizeof(int16_t), &done);
        return done / sizeof(int16_t);
    }
#endif
    
#ifdef USE_VORBIS
    case FORMAT_OGG: {
        int read = 0, sec;
        while (read < max_samps) {
            long ret = ov_read(&d->vorbis_file, (char*)(buf + read),
                               (max_samps - read) * sizeof(int16_t), 0, 2, 1, &sec);
            if (ret <= 0) break;
            read += ret / sizeof(int16_t);
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
    
    if (d->format == FORMAT_SPX && d->buffer) {
        spx_decoder_data_t *spx_data = (spx_decoder_data_t*)d->buffer;
        free(spx_data->history);
        free(spx_data->compressed_buffer);
        free(spx_data->temp_buffer);
        free(spx_data);
    } else if (d->buffer) {
        free(d->buffer);
    }
    
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
    
    free(d);
}