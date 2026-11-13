#pragma once
/* The game disc image the Vita starts with: where it was looked for, which one was taken, and the
 * one problem to report when none fits (disc_locate.cpp). Plain C types only: main_vita.cpp (SDK
 * short-enum domain) and start-up screens call it; the disc code is built with int-sized enums.
 *
 * Search order (the first image that is a supported Twilight Princess GameCube disc wins):
 *   1. the configured image: "backend.isoPath" in config.json (Dusklight's setting, which it sets
 *      to the image it last opened), when set;
 *   2. ux0:data/twilight-vita/game.iso;
 *   3. any other image in ux0:data/twilight-vita/ (.iso .gcm .ciso .gcz, and the formats only
 *      named: .rvz .wia .wbfs .nfs .tgc .cso), in name order;
 *   4. the same in ux0:data/dusklight/ (Zelda.iso or Zelda.ciso, where another Vita build keeps it);
 *   5. the same in data/twilight-vita/ on uma0:, imc0: and xmc0: (a second storage device).
 * Images are read in place, never copied. The region comes from the disc header, not the name. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TWV_DISC_OK 0
#define TWV_DISC_NO_IMAGE 1          /* no file at any searched place */
#define TWV_DISC_UNREADABLE 2        /* a file that cannot be opened or read */
#define TWV_DISC_UNSUPPORTED_FORMAT 3 /* RVZ, WIA, WBFS, NFS, TGC, CSO, NKit, or a damaged CISO/GCZ */
#define TWV_DISC_NOT_GAMECUBE 4      /* no GameCube or Wii disc magic */
#define TWV_DISC_WRONG_DISC 5        /* a disc, but not GZ2E01, GZ2P01 or GZ2J01 (Wii discs too) */

#define TWV_DISC_PATH_BYTES 192
struct TwvDiscReport {
    int status;                      /* TWV_DISC_* */
    char path[TWV_DISC_PATH_BYTES];  /* the image taken, or the one the problem is about */
    char format[8];                  /* "ISO", "CISO", "GCZ", or the unsupported one ("RVZ") */
    char game_id[8];                 /* "GZ2P01" (header bytes 0-5), "" when unknown */
    char region;                     /* game ID letter 4: 'E' USA, 'P' Europe, 'J' Japan, 0 unknown */
    int revision;                    /* header byte 7 */
    unsigned long long file_bytes;   /* the file */
    unsigned long long disc_bytes;   /* the logical disc behind it */
    unsigned block_bytes;            /* CISO/GCZ block size, 0 for ISO */
    char detail[160];                /* the reader's message for a problem, else "" */
    char searched[768];              /* every place looked at: "path=status;..." (receipts) */
    int candidates;                  /* files probed */
};

/* One file. Returns status (also in out->status). */
int twv_disc_probe(const char* path, struct TwvDiscReport* out);
/* The search above; `configured` may be NULL or "". Returns status. */
int twv_disc_find(const char* configured, struct TwvDiscReport* out);
/* Up to `len` bytes of the logical disc at `offset`, read through the image's container (the
 * start-up check of the chosen image's file table). Returns the count, or -1. */
long long twv_disc_read(const char* path, unsigned long long offset, void* out, unsigned len);
/* "ok", "no_image", "unreadable", "unsupported_format", "not_gamecube", "wrong_disc". */
const char* twv_disc_status_name(int status);
/* Card folder Aurora uses for the region letter (dolphin/card.cpp): "USA", "EUR", "JAP". */
const char* twv_disc_card_folder(char region);

/* Read speed through the container reader (--vita-disc-read-test): `mib` MiB from the start of the
 * logical disc, in `chunk_bytes` requests. Returns 1 on success. */
struct TwvDiscReadTest {
    unsigned long long bytes;
    unsigned long long micros;
    unsigned long long blocks_inflated;
};
int twv_disc_read_test(const char* path, unsigned mib, unsigned chunk_bytes, struct TwvDiscReadTest* out);

#ifdef __cplusplus
}

namespace twv::disc {
/* The search over other folders (host tests): folders[0] holds `defaultImage` (its file name). */
int find_in(const char* configured, const char* defaultImage, const char* const* folders, int folderCount,
            TwvDiscReport* out);
}
#endif
