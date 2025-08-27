// mkcdda, a minimalist WAV to CDDA (BIN/CUE) converter
// Eduardo Meli 2025

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2352

static unsigned short rd_le_u16(const unsigned char *b) {
  return (unsigned short)(b[0] | (b[1] << 8));
}
static unsigned long rd_le_u32(const unsigned char *b) {
  return (unsigned long)((unsigned long)b[0] | ((unsigned long)b[1] << 8) |
                         ((unsigned long)b[2] << 16) |
                         ((unsigned long)b[3] << 24));
}

static void mmssff_from_sectors(unsigned long sectors, char *out) {
  unsigned long minutes = sectors / (75UL * 60UL);
  unsigned long seconds = (sectors / 75UL) % 60UL;
  unsigned long frames = sectors % 75UL;
  sprintf(out, "%02lu:%02lu:%02lu", minutes, seconds, frames);
}

static int parse_wav(FILE *f, const char *name, unsigned long *data_ofs,
                     unsigned long *data_size) {
  unsigned char hdr12[12];
  int got_fmt = 0, got_data = 0;
  unsigned short audioFormat = 0, numChannels = 0, bitsPerSample = 0;
  unsigned long sampleRate = 0;

  if (fread(hdr12, 1, 12, f) != 12) {
    fprintf(stderr, "%s: cannot read RIFF header\n", name);
    return -1;
  }
  if (memcmp(hdr12 + 0, "RIFF", 4) != 0 || memcmp(hdr12 + 8, "WAVE", 4) != 0) {
    fprintf(stderr, "%s: not a RIFF/WAVE file\n", name);
    return -1;
  }

  for (;;) {
    unsigned char chdr[8];
    unsigned long csize;

    if (fread(chdr, 1, 8, f) != 8)
      break;
    csize = rd_le_u32(chdr + 4);

    if (memcmp(chdr, "fmt ", 4) == 0) {
      unsigned char fmbuf[40];
      size_t need = csize < sizeof(fmbuf) ? csize : sizeof(fmbuf);
      if (fread(fmbuf, 1, need, f) != need)
        return -1;
      if (csize >= 16) {
        audioFormat = rd_le_u16(fmbuf + 0);
        numChannels = rd_le_u16(fmbuf + 2);
        sampleRate = rd_le_u32(fmbuf + 4);
        bitsPerSample = rd_le_u16(fmbuf + 14);
        got_fmt = 1;
      }
      if ((long)need < (long)csize) {
        if (fseek(f, (long)csize - (long)need, SEEK_CUR) != 0)
          return -1;
      }
      if (csize & 1UL) {
        if (fseek(f, 1L, SEEK_CUR) != 0)
          return -1;
      }
    } else if (memcmp(chdr, "data", 4) == 0) {
      long here = ftell(f);
      if (here < 0)
        return -1;
      *data_ofs = (unsigned long)here;
      *data_size = csize;
      if (fseek(f, (long)csize, SEEK_CUR) != 0)
        return -1;
      if (csize & 1UL) {
        if (fseek(f, 1L, SEEK_CUR) != 0)
          return -1;
      }
      got_data = 1;
    } else {
      if (fseek(f, (long)csize, SEEK_CUR) != 0)
        return -1;
      if (csize & 1UL) {
        if (fseek(f, 1L, SEEK_CUR) != 0)
          return -1;
      }
    }

    if (got_fmt && got_data)
      break;
  }

  if (!got_fmt || !got_data) {
    fprintf(stderr, "%s: missing fmt or data chunk\n", name);
    return -1;
  }
  if (!(audioFormat == 1 && numChannels == 2 && sampleRate == 44100UL &&
        bitsPerSample == 16)) {
    fprintf(stderr, "%s must be 44.1kHz, 16-bit, stereo PCM\n", name);
    return -1;
  }
  return 0;
}

int main(int argc, char *argv[]) {
  const char *bin_path = "disc.bin";
  const char *cue_path = "disc.cue";
  FILE *fbin = NULL;
  FILE *fcue = NULL;
  unsigned long *index_times = NULL;
  unsigned long total_sectors_before = 0UL;
  int i, ntracks;

  if (argc < 2) {
    fprintf(stderr, "No track*.wav files found (pass them as arguments).\n");
    fprintf(stderr, "Usage: %s track1.wav [track2.wav ...]\n", argv[0]);
    return 1;
  }

  ntracks = argc - 1;
  fbin = fopen(bin_path, "wb");
  if (!fbin) {
    perror("open disc.bin");
    return 1;
  }

  index_times =
      (unsigned long *)malloc(sizeof(unsigned long) * (size_t)ntracks);
  if (!index_times) {
    fprintf(stderr, "Out of memory\n");
    fclose(fbin);
    return 1;
  }

  for (i = 0; i < ntracks; ++i) {
    const char *wav = argv[1 + i];
    FILE *fin = fopen(wav, "rb");
    unsigned long data_ofs = 0, data_size = 0, remaining;
    unsigned long padded, pad_bytes;
    unsigned char buf[8192];
    size_t nread;

    if (!fin) {
      perror("open wav");
      free(index_times);
      fclose(fbin);
      return 1;
    }

    if (parse_wav(fin, wav, &data_ofs, &data_size) != 0) {
      fclose(fin);
      free(index_times);
      fclose(fbin);
      return 1;
    }

    index_times[i] = total_sectors_before;

    if (fseek(fin, (long)data_ofs, SEEK_SET) != 0) {
      fprintf(stderr, "%s: seek failed\n", wav);
      fclose(fin);
      free(index_times);
      fclose(fbin);
      return 1;
    }
    remaining = data_size;
    while (remaining > 0) {
      size_t chunk =
          (remaining > sizeof(buf)) ? sizeof(buf) : (size_t)remaining;
      nread = fread(buf, 1, chunk, fin);
      if (nread != chunk) {
        fprintf(stderr, "%s: short read\n", wav);
        fclose(fin);
        free(index_times);
        fclose(fbin);
        return 1;
      }
      if (fwrite(buf, 1, chunk, fbin) != chunk) {
        fprintf(stderr, "disc.bin: write error\n");
        fclose(fin);
        free(index_times);
        fclose(fbin);
        return 1;
      }
      remaining -= (unsigned long)chunk;
    }
    fclose(fin);

    // zero pad to 2352-byte boundary
    pad_bytes = (unsigned long)((SECTOR - (data_size % SECTOR)) % SECTOR);
    if (pad_bytes) {
      memset(buf, 0, sizeof(buf));
      while (pad_bytes > 0) {
        size_t chunk2 =
            (pad_bytes > sizeof(buf)) ? sizeof(buf) : (size_t)pad_bytes;
        if (fwrite(buf, 1, chunk2, fbin) != chunk2) {
          fprintf(stderr, "disc.bin: write error (padding)\n");
          free(index_times);
          fclose(fbin);
          return 1;
        }
        pad_bytes -= (unsigned long)chunk2;
      }
    }

    padded = data_size + ((SECTOR - (data_size % SECTOR)) % SECTOR);
    total_sectors_before += padded / SECTOR;

    printf("Appended %s (%lu bytes, padded to %lu)\n", wav, data_size, padded);
  }

  fcue = fopen(cue_path, "wb");
  if (!fcue) {
    perror("open disc.cue");
    free(index_times);
    fclose(fbin);
    return 1;
  }

  {
    char line[256];
    char ts[16];
    sprintf(line, "FILE \"%s\" BINARY\n", bin_path);
    fwrite(line, 1, strlen(line), fcue);

    for (i = 0; i < ntracks; ++i) {
      sprintf(line, "  TRACK %02d AUDIO\n", i + 1);
      fwrite(line, 1, strlen(line), fcue);
      if (i == 0) {
        // 2s gap only on CUE, not actual silent PCM
        strcpy(line, "    PREGAP 00:02:00\n");
        fwrite(line, 1, strlen(line), fcue);
      }
      mmssff_from_sectors(index_times[i], ts);
      sprintf(line, "    INDEX 01 %s\n", ts);
      fwrite(line, 1, strlen(line), fcue);
    }
  }

  fclose(fcue);
  fclose(fbin);
  free(index_times);

  printf("Done! Created %s and %s with %d track(s).\n", bin_path, cue_path,
         ntracks);
  return 0;
}
