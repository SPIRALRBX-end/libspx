// codec antigo
#include "spx_codec.h"
#include "spx_metadata.h"
#include "spx_decoder.h"
#include "spx_encoder.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>    // unlink()
#include <libgen.h>    // basename()
#include <sys/stat.h>
#include <errno.h>
#include <time.h>      // time() function for srand()

#ifdef USE_MPG123
#include <lame/lame.h>
#endif

#ifdef USE_VORBIS
#include <vorbis/vorbisenc.h>
#endif

#define FRAME_SIZE   320
#define MAX_CHANNELS 2

static inline int16_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
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
    // Cabeçalho RIFF/WAVE PCM 16‑bit
    typedef struct {
        char     riff[4];      // "RIFF"
        uint32_t overall_size; // 36 + data_sz
        char     wave[4];      // "WAVE"
        char     fmt_chunk[4]; // "fmt "
        uint32_t fmt_len;      // 16
        uint16_t audio_fmt;    // 1 = PCM
        uint16_t num_ch;       // canais
        uint32_t sample_rate;  // ex: 44100
        uint32_t byte_rate;    // sample_rate * num_ch * bits/8
        uint16_t block_align;  // num_ch * bits/8
        uint16_t bits_per_samp;// 16
        char     data_chunk[4];// "data"
        uint32_t data_size;    // data_sz
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
static int encode_wav_to_mp3(const char *wavfile, const char *mp3file) {
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
    
    // Configurar LAME (assumindo stereo 44100Hz)
    lame_set_in_samplerate(gfp, 44100);
    lame_set_num_channels(gfp, 2);
    lame_set_quality(gfp, 2);  // 0=melhor qualidade, 9=pior
    lame_set_brate(gfp, 128);  // bitrate 128 kbps
    
    if (lame_init_params(gfp) < 0) {
        fprintf(stderr, "Erro configurando LAME\n");
        lame_close(gfp);
        fclose(wav); fclose(mp3);
        return -1;
    }
    
    // Buffer para leitura/escrita
    const int PCM_SIZE = 8192;
    const int MP3_SIZE = 8192;
    short int pcm_buffer[PCM_SIZE * 2];  // stereo
    unsigned char mp3_buffer[MP3_SIZE];
    
    int read, write;
    while ((read = fread(pcm_buffer, sizeof(short int), PCM_SIZE * 2, wav)) > 0) {
        write = lame_encode_buffer_interleaved(gfp, pcm_buffer, read/2, 
                                             mp3_buffer, MP3_SIZE);
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
static int encode_wav_to_ogg(const char *wavfile, const char *oggfile) {
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
    
    // Configurar: stereo, 44100Hz, quality 0.5
    if (vorbis_encode_init_vbr(&vi, 2, 44100, 0.5) != 0) {
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
    const int READ_SIZE = 4096;
    short readbuffer[READ_SIZE * 2];  // stereo
    int eos = 0;
    
    while (!eos) {
        int bytes = fread(readbuffer, sizeof(short), READ_SIZE * 2, wav);
        if (bytes == 0) {
            vorbis_analysis_wrote(&vd, 0);
        } else {
            float **buffer = vorbis_analysis_buffer(&vd, bytes/2);
            
            // Converter de interleaved short para float separado
            for (int i = 0; i < bytes/2; i++) {
                buffer[0][i] = readbuffer[i*2] / 32768.0f;
                buffer[1][i] = readbuffer[i*2+1] / 32768.0f;
            }
            
            vorbis_analysis_wrote(&vd, bytes/2);
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
    /* (igual ao original) */
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
    uint8_t buf[FRAME_SIZE * MAX_CHANNELS * sizeof(int16_t)];
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
    /* (igual ao original, chama write_wav_header corretamente) */
    FILE *in = fopen(spxfile, "rb");
    if (!in) { perror("Abrir SPX"); return -1; }

    int sr, ch;
    if (read_spx_header(in, &sr, &ch) < 0) {
        fprintf(stderr,"SPX inválido\n");
        fclose(in);
        return -1;
    }

    // pula META
    char tag[4];
    fread(tag,1,4,in);
    if (memcmp(tag,"META",4)==0) {
        uint32_t msz;
        fread(&msz,4,1,in);
        fseek(in, msz, SEEK_CUR);
    } else {
        fseek(in, -4, SEEK_CUR);
    }

    FILE *out = fopen(wavfile, "wb");
    if (!out) { perror("Criar WAV"); fclose(in); return -1; }

    // reserva espaço (header será regravado)
    fseek(out, 44, SEEK_SET);

    int16_t delta[FRAME_SIZE*MAX_CHANNELS], pcm[FRAME_SIZE*MAX_CHANNELS];
    int16_t last[MAX_CHANNELS] = {0};
    uint32_t total_samps = 0;
    size_t block = sizeof(int16_t) * FRAME_SIZE * ch;

    while (fread(delta, 1, block, in) == block) {
        for (int i = 0; i < FRAME_SIZE*ch; i++) {
            int c = i % ch;
            int32_t v = last[c] + delta[i];
            pcm[i] = clamp16(v);
            last[c] = pcm[i];
        }
        fwrite(pcm, sizeof(int16_t), FRAME_SIZE*ch, out);
        total_samps += FRAME_SIZE*ch;
    }

    // escreve o header correto
    fseek(out, 0, SEEK_SET);
    write_wav_header(out, sr, ch, total_samps * sizeof(int16_t));

    fclose(in);
    fclose(out);
    return 0;
}

int spx_to_mp3(const char *spxfile, const char *mp3file) {
#ifdef USE_MPG123
    char tmpname[512];
    
    // Criar nome temporário
#ifdef _WIN32
    const char *tempdir = getenv("TEMP");
    if (!tempdir) tempdir = ".";
    snprintf(tmpname, sizeof(tmpname),
             "%s\\spxconv_temp_%llu.wav",
             tempdir, (unsigned long long)time(NULL));
#else
    char template[] = "/tmp/spxconvXXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) { perror("mkstemp"); return -1; }
    close(fd);
    snprintf(tmpname, sizeof(tmpname), "%s.wav", template);
    if (rename(template, tmpname) != 0) {
        perror("rename temp wav");
        unlink(template);
        return -1;
    }
#endif
    
    // Decodificar SPX para WAV temporário
    if (spx_to_wav(spxfile, tmpname) != 0) {
        unlink(tmpname);
        return -1;
    }
    
    // Codificar WAV para MP3 usando LAME
    int result = encode_wav_to_mp3(tmpname, mp3file);
    
    // Apagar WAV temporário
    unlink(tmpname);
    return result;
#else
    fprintf(stderr, "Erro: suporte ao MP3 não compilado (USE_MPG123 não definido)\n");
    return -1;
#endif
}

int spx_to_ogg(const char *spxfile, const char *oggfile) {
#ifdef USE_VORBIS
    char tmpname[512];
    
    // Criar nome temporário
#ifdef _WIN32
    const char *tempdir = getenv("TEMP");
    if (!tempdir) tempdir = ".";
    snprintf(tmpname, sizeof(tmpname),
             "%s\\spxconv_temp_%llu.wav",
             tempdir, (unsigned long long)time(NULL));
#else
    char template[] = "/tmp/spxconvXXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) { perror("mkstemp"); return -1; }
    close(fd);
    snprintf(tmpname, sizeof(tmpname), "%s.wav", template);
    if (rename(template, tmpname) != 0) {
        perror("rename temp wav");
        unlink(template);
        return -1;
    }
#endif
    
    // Decodificar SPX para WAV temporário
    if (spx_to_wav(spxfile, tmpname) != 0) {
        unlink(tmpname);
        return -1;
    }
    
    // Codificar WAV para OGG usando libvorbis
    int result = encode_wav_to_ogg(tmpname, oggfile);
    
    // Apagar WAV temporário
    unlink(tmpname);
    return result;
#else
    fprintf(stderr, "Erro: suporte ao OGG não compilado (USE_VORBIS não definido)\n");
    return -1;
#endif
}