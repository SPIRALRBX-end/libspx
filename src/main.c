#include "spx_codec.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
  #include <windows.h>
  #define PATH_SEP '\\'
#else
  #include <sys/stat.h>
  #include <fcntl.h>
#endif

static const char *get_ext(const char *fn) {
    const char *dot = strrchr(fn, '.');
    return dot ? dot + 1 : "";
}

static void to_lowercase(char *s) {
    for (; *s; ++s) *s = tolower(*s);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr,
            "Uso:\n"
            "  %s -e in.[wav|mp3|ogg] out.spx\n"
            "  %s -d in.spx     out.[wav|mp3|ogg]\n",
            argv[0], argv[0]
        );
        return 1;
    }

    if (strcmp(argv[1], "-e") == 0) {
        return audio_to_spx(argv[2], argv[3]);
    }
    else if (strcmp(argv[1], "-d") == 0) {
        const char *infile = argv[2];
        const char *outfile = argv[3];
        char ext_buf[16];
        strncpy(ext_buf, get_ext(outfile), sizeof(ext_buf)-1);
        ext_buf[sizeof(ext_buf)-1] = '\0';
        to_lowercase(ext_buf);

        if (strcmp(ext_buf, "wav") == 0) {
            return spx_to_wav(infile, outfile);
        }
        else if (strcmp(ext_buf, "mp3") == 0) {
            return spx_to_mp3(infile, outfile);
        }
        else if (strcmp(ext_buf, "ogg") == 0) {
            return spx_to_ogg(infile, outfile);
        }
        else {
            fprintf(stderr, "Erro: formato de saída '%s' não suportado\n", ext_buf);
            return 1;
        }
    }

    fprintf(stderr,"Modo inválido\n");
    return 1;
}