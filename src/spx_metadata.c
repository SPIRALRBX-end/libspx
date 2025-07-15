#include "spx_metadata.h"
#include <string.h>
#include <sys/stat.h>

int read_id3v1(const char *fn, metadata_t *m) {
    FILE *f = fopen(fn, "rb");
    if (!f) return -1;
    if (fseek(f, -ID3V1_TAG_SIZE, SEEK_END) != 0) { fclose(f); return -1; }
    uint8_t buf[ID3V1_TAG_SIZE];
    if (fread(buf,1,ID3V1_TAG_SIZE,f) != ID3V1_TAG_SIZE) { fclose(f); return -1; }
    fclose(f);
    if (memcmp(buf, "TAG", 3) != 0) return -1;
    memcpy(m->title,   buf+3,  30); m->title[30]=0;
    memcpy(m->artist,  buf+33, 30); m->artist[30]=0;
    memcpy(m->album,   buf+63, 30); m->album[30]=0;
    memcpy(m->year,    buf+93,  4); m->year[4]=0;
    memcpy(m->comment, buf+97, 30); m->comment[30]=0;
    m->genre = buf[127];
    m->track[0] = m->album_artist[0] = 0;
    m->rating = 0;
    return 0;
}

void calc_audio_props(const char *fn,
                      int sample_rate,
                      int total_samples,
                      double *out_secs,
                      uint32_t *out_bitrate)
{
    struct stat st;
    double secs = 0;
    uint32_t br = 0;
    if (stat(fn, &st)==0 && sample_rate>0) {
        secs = (double)total_samples / sample_rate;
        if (secs > 0) br = (uint32_t)((st.st_size * 8) / secs);
    }
    *out_secs = secs;
    *out_bitrate = br;
}

void write_meta_chunk(FILE *out,
                      const metadata_t *m,
                      double length_s,
                      uint32_t bitrate,
                      const char *infile)
{
    struct { const char *k, *v; } P[] = {
        {"Titulo",      m->title},
        {"Legenda",     m->comment},
        {"Classificacao", m->rating ? (char[]){m->rating,0} : ""},
        {"Comentario",  m->comment},
        {"ArtistaParticipante", m->artist},
        {"ArtistaAlbum",        m->album_artist},
        {"Album",               m->album},
        {"Ano",                 m->year},
        {"NumeroFaixa",         m->track},
        {"Genero",              m->genre ? (char[]){m->genre,0} : ""},
        {"Comprimento",         ""},
        {"TaxaBits",            ""},
        {"Origem",              infile},
        { NULL, NULL }
    };

    char buf_len[32], buf_br[32];
    snprintf(buf_len, sizeof(buf_len), "%.0f", length_s);
    snprintf(buf_br,  sizeof(buf_br), "%u", bitrate);

    for (int i = 0; P[i].k; i++) {
        if (strcmp(P[i].k, "Comprimento")==0) P[i].v = buf_len;
        if (strcmp(P[i].k, "TaxaBits")==0)    P[i].v = buf_br;
    }

    uint32_t total = 0;
    for (int i = 0; P[i].k; i++) {
        if (*P[i].v)
            total += strlen(P[i].k)+1 + strlen(P[i].v)+1;
    }

    fwrite("META", 1, 4, out);
    fwrite(&total, sizeof(total), 1, out);
    for (int i = 0; P[i].k; i++) {
        if (*P[i].v) {
            fwrite(P[i].k, 1, strlen(P[i].k)+1, out);
            fwrite(P[i].v, 1, strlen(P[i].v)+1, out);
        }
    }
}