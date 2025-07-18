#include "spx_codec.h"
#include "spx_metadata.h"
#include "spx_decoder.h"
#include "spx_encoder.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#ifdef USE_MPG123
#include <lame/lame.h>
#endif

#ifdef USE_VORBIS
#include <vorbis/vorbisenc.h>
#endif

#define FRAME_SIZE   320
#define MAX_CHANNELS 2

// Tabela de quantização (mesmo do encoder/decoder)
static const int16_t quant_table[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28,
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448,
    512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096
};

#define QUANT_LEVELS (sizeof(quant_table)/sizeof(quant_table[0]))

static inline int16_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
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

// Decompressão RLE
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

void write_spx_header(FILE *f, int sr, int ch) {
    fwrite("SPX3", 4, 1, f);
    fwrite(&sr, sizeof(sr), 1, f);
    fwrite(&ch, sizeof(ch), 1, f);
    uint32_t bps = 16;
    fwrite(&bps, sizeof(bps), 1, f);
}

int read_spx_header(FILE *f, int *sr, int *ch) {
    char magic[4];
    if (fread(magic,4,1,f)!=1 || memcmp(magic,"SPX3",4)!=0) return -1;
    uint32_t bps;
    fread(sr, sizeof(*sr), 1, f);
    fread(ch, sizeof(*ch), 1, f);
    fread(&bps, sizeof(bps), 1, f);
    if (bps != 16) { fprintf(stderr,"SPX: só 16 bits suportado\n"); return -1; }
    return 0;
}

void write_wav_header(FILE *f, int sr, int ch, uint32_t data_sz) {
    typedef struct {
        char     riff[4];
        uint32_t overall_size;
        char     wave[4];
        char     fmt_chunk[4];
        uint32_t fmt_len;
        uint16_t audio_fmt;
        uint16_t num_ch;
        uint32_t sample_rate;
        uint32_t byte_rate;
        uint16_t block_align;
        uint16_t bits_per_samp;
        char     data_chunk[4];
        uint32_t data_size;
    } wav_hdr_t;

    wav_hdr_t h;
    memcpy(h.riff,       "RIFF", 4);
    h.overall_size      = 36 + data_sz;
    memcpy(h.wave,       "WAVE", 4);
    memcpy(h.fmt_chunk,  "fmt ", 4);
    h.fmt_len           = 16;
    h.audio_fmt         = 1;
    h.num_ch            = ch;
    h.sample_rate       = sr;
    h.bits_per_samp     = 16;
    h.byte_rate         = sr * ch * (h.bits_per_samp/8);
    h.block_align       = ch * (h.bits_per_samp/8);
    memcpy(h.data_chunk, "data", 4);
    h.data_size         = data_sz;

    fwrite(&h, sizeof(h), 1, f);
}

#ifdef USE_MPG123
static int encode_wav_to_mp3(const char *wavfile, const char *mp3file, int sr, int ch) {
    FILE *wav = fopen(wavfile, "rb");
    if (!wav) { perror("Abrir WAV para MP3"); return -1; }
    
    FILE *mp3 = fopen(mp3file, "wb");
    if (!mp3) { perror("Criar MP3"); fclose(wav); return -1; }
    
    // Pular header WAV (44 bytes)
    fseek(wav, 44, SEEK_SET);
    
    // Inicializar LAME
    lame_global_flags *gfp = lame_init();
    if (!gfp) {
        fprintf(stderr, "Erro inicializando LAME\n");
        fclose(wav); fclose(mp3);
        return -1;
    }
    
    // Configurar LAME
    lame_set_in_samplerate(gfp, sr);
    lame_set_num_channels(gfp, ch);
    lame_set_quality(gfp, 2);
    lame_set_brate(gfp, 128);
    
    if (lame_init_params(gfp) < 0) {
        fprintf(stderr, "Erro configurando LAME\n");
        lame_close(gfp);
        fclose(wav); fclose(mp3);
        return -1;
    }
    
    // Buffer para leitura/escrita
    const int PCM_SIZE = 1152;
    const int MP3_SIZE = 1152 * 4 + 7200;
    int16_t pcm_buffer[PCM_SIZE * MAX_CHANNELS];
    unsigned char mp3_buffer[MP3_SIZE];
    
    int read, write;
    while ((read = fread(pcm_buffer, sizeof(int16_t), PCM_SIZE * ch, wav)) > 0) {
        if (ch == 1) {
            write = lame_encode_buffer(gfp, pcm_buffer, NULL, read, mp3_buffer, MP3_SIZE);
        } else {
            write = lame_encode_buffer_interleaved(gfp, pcm_buffer, read/ch, mp3_buffer, MP3_SIZE);
        }
        
        if (write < 0) {
            fprintf(stderr, "Erro codificando MP3\n");
            break;
        }
        fwrite(mp3_buffer, 1, write, mp3);
    }
    
    // Flush final
    write = lame_encode_flush(gfp, mp3_buffer, MP3_SIZE);
    if (write > 0) {
        fwrite(mp3_buffer, 1, write, mp3);
    }
    
    lame_close(gfp);
    fclose(wav);
    fclose(mp3);
    return 0;
}
#endif

#ifdef USE_VORBIS
static int encode_wav_to_ogg(const char *wavfile, const char *oggfile, int sr, int ch) {
    FILE *wav = fopen(wavfile, "rb");
    if (!wav) { perror("Abrir WAV para OGG"); return -1; }
    
    FILE *ogg = fopen(oggfile, "wb");
    if (!ogg) { perror("Criar OGG"); fclose(wav); return -1; }
    
    // Pular header WAV (44 bytes)
    fseek(wav, 44, SEEK_SET);
    
    // Inicializar Vorbis encoder
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    
    vorbis_info_init(&vi);
    
    // Configurar: usando sample rate e channels corretos
    if (vorbis_encode_init_vbr(&vi, ch, sr, 0.5) != 0) {
        fprintf(stderr, "Erro inicializando Vorbis encoder\n");
        fclose(wav); fclose(ogg);
        return -1;
    }
    
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "ENCODER", "spxconv");
    
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    
    // Configurar Ogg stream
    ogg_stream_state os;
    ogg_page og;
    ogg_packet op;
    
    srand(time(NULL));
    ogg_stream_init(&os, rand());
    
    // Escrever headers
    ogg_packet header;
    ogg_packet header_comm;
    ogg_packet header_code;
    
    vorbis_analysis_headerout(&vd, &vc, &header, &header_comm, &header_code);
    ogg_stream_packetin(&os, &header);
    ogg_stream_packetin(&os, &header_comm);
    ogg_stream_packetin(&os, &header_code);
    
    while (ogg_stream_flush(&os, &og) != 0) {
        fwrite(og.header, 1, og.header_len, ogg);
        fwrite(og.body, 1, og.body_len, ogg);
    }
    
    // Codificar dados
    const int READ_SIZE = 1024;
    int16_t readbuffer[READ_SIZE * MAX_CHANNELS];
    int eos = 0;
    
    while (!eos) {
        int bytes = fread(readbuffer, sizeof(int16_t), READ_SIZE * ch, wav);
        if (bytes == 0) {
            vorbis_analysis_wrote(&vd, 0);
        } else {
            float **buffer = vorbis_analysis_buffer(&vd, bytes/ch);
            
            // Converter de interleaved int16 para float separado
            for (int i = 0; i < bytes/ch; i++) {
                for (int c = 0; c < ch; c++) {
                    buffer[c][i] = readbuffer[i*ch + c] / 32768.0f;
                }
            }
            
            vorbis_analysis_wrote(&vd, bytes/ch);
        }
        
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, NULL);
            vorbis_bitrate_addblock(&vb);
            
            while (vorbis_bitrate_flushpacket(&vd, &op)) {
                ogg_stream_packetin(&os, &op);
                
                while (!eos) {
                    int result = ogg_stream_pageout(&os, &og);
                    if (result == 0) break;
                    
                    fwrite(og.header, 1, og.header_len, ogg);
                    fwrite(og.body, 1, og.body_len, ogg);
                    
                    if (ogg_page_eos(&og)) eos = 1;
                }
            }
        }
    }
    
    // Cleanup
    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    
    fclose(wav);
    fclose(ogg);
    return 0;
}
#endif

int audio_to_spx(const char *infile, const char *outfile) {
    metadata_t m;
    memset(&m, 0, sizeof(m));
    read_id3v1(infile, &m);

    audio_decoder_t *dec = create_decoder(infile);
    if (!dec) { fprintf(stderr,"Erro abrindo %s\n", infile); return -1; }
    FILE *out = fopen(outfile, "wb");
    if (!out) { perror("Criar SPX"); destroy_decoder(dec); return -1; }

    write_spx_header(out, dec->sample_rate, dec->channels);

    double dur_s; uint32_t br;
    calc_audio_props(infile, dec->sample_rate, dec->total_samples, &dur_s, &br);
    write_meta_chunk(out, &m, dur_s, br, infile);

    spx_encoder_t *enc = spx_encoder_create(dec->sample_rate, dec->channels);
    int16_t pcm[FRAME_SIZE * MAX_CHANNELS];
    uint8_t buf[FRAME_SIZE * MAX_CHANNELS * 2];
    int samples_per_frame = FRAME_SIZE * dec->channels;

    while (1) {
        int got = read_samples(dec, pcm, samples_per_frame);
        if (got <= 0) break;
        if (got < samples_per_frame)
            memset(pcm + got, 0, (samples_per_frame - got)*sizeof(int16_t));
        int w = spx_encode_frame(enc, pcm, got/dec->channels, buf);
        fwrite(buf, 1, w, out);
    }

    spx_encoder_destroy(enc);
    fclose(out);
    destroy_decoder(dec);
    return 0;
}

int spx_to_wav(const char *spxfile, const char *wavfile) {
    audio_decoder_t *dec = create_decoder(spxfile);
    if (!dec) { 
        fprintf(stderr,"Erro abrindo %s\n", spxfile); 
        return -1; 
    }

    FILE *out = fopen(wavfile, "wb");
    if (!out) { 
        perror("Criar WAV"); 
        destroy_decoder(dec); 
        return -1; 
    }

    // Reserva espaço para o header
    fseek(out, 44, SEEK_SET);
    
    uint32_t total_bytes = 0;
    int16_t pcm[FRAME_SIZE * MAX_CHANNELS];
    
    while (1) {
        int got = read_samples(dec, pcm, FRAME_SIZE * dec->channels);
        if (got <= 0) break;
        
        fwrite(pcm, sizeof(int16_t), got, out);
        total_bytes += got * sizeof(int16_t);
    }

    // Escreve o header correto no início
    fseek(out, 0, SEEK_SET);
    write_wav_header(out, dec->sample_rate, dec->channels, total_bytes);

    fclose(out);
    destroy_decoder(dec);
    return 0;
}

int spx_to_mp3(const char *spxfile, const char *mp3file) {
#ifdef USE_MPG123
    char tmpname[512];
    
    // Primeiro, obter informações do arquivo SPX
    FILE *spx_file = fopen(spxfile, "rb");
    if (!spx_file) {
        perror("Abrir SPX para MP3");
        return -1;
    }
    
    int sr, ch;
    if (read_spx_header(spx_file, &sr, &ch) < 0) {
        fprintf(stderr, "SPX inválido para MP3\n");
        fclose(spx_file);
        return -1;
    }
    fclose(spx_file);
    
#ifdef _WIN32
    const char *tempdir = getenv("TEMP");
    if (!tempdir) tempdir = ".";
    snprintf(tmpname, sizeof(tmpname),
             "%s\\spxconv_temp_%llu.wav",
             tempdir, (unsigned long long)time(NULL));
#else
    char template[] = "/tmp/spxconvXXXXXX";
    int fd = mkstemp(template);
    if (fd == -1) {
        perror("Criar arquivo temporário");
        return -1;
    }
    close(fd);
    strcpy(tmpname, template);
    strcat(tmpname, ".wav");
#endif

    // Converte SPX para WAV temporário
    if (spx_to_wav(spxfile, tmpname) < 0) {
        fprintf(stderr, "Erro convertendo SPX para WAV\n");
        return -1;
    }

    // Converte WAV para MP3 com as informações corretas
    int result = encode_wav_to_mp3(tmpname, mp3file, sr, ch);

    // Remove arquivo temporário
    unlink(tmpname);
    
    return result;
#else
    fprintf(stderr, "Suporte a MP3 não compilado\n");
    return -1;
#endif
}

int spx_to_ogg(const char *spxfile, const char *oggfile) {
#ifdef USE_VORBIS
    char tmpname[512];
    
    // Primeiro, obter informações do arquivo SPX
    FILE *spx_file = fopen(spxfile, "rb");
    if (!spx_file) {
        perror("Abrir SPX para OGG");
        return -1;
    }
    
    int sr, ch;
    if (read_spx_header(spx_file, &sr, &ch) < 0) {
        fprintf(stderr, "SPX inválido para OGG\n");
        fclose(spx_file);
        return -1;
    }
    fclose(spx_file);
    
#ifdef _WIN32
    const char *tempdir = getenv("TEMP");
    if (!tempdir) tempdir = ".";
    snprintf(tmpname, sizeof(tmpname),
             "%s\\spxconv_temp_%llu.wav",
             tempdir, (unsigned long long)time(NULL));
#else
    char template[] = "/tmp/spxconvXXXXXX";
    int fd = mkstemp(template);
    if (fd == -1) {
        perror("Criar arquivo temporário");
        return -1;
    }
    close(fd);
    strcpy(tmpname, template);
    strcat(tmpname, ".wav");
#endif

    // Converte SPX para WAV temporário
    if (spx_to_wav(spxfile, tmpname) < 0) {
        fprintf(stderr, "Erro convertendo SPX para WAV\n");
        return -1;
    }

    // Converte WAV para OGG com as informações corretas
    int result = encode_wav_to_ogg(tmpname, oggfile, sr, ch);

    // Remove arquivo temporário
    unlink(tmpname);
    
    return result;
#else
    fprintf(stderr, "Suporte a OGG não compilado\n");
    return -1;
#endif
}