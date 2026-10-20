/*
 * rom_validate.c - check that a file is a ROM this recompilation can use.
 *
 * The recompilation is built for one cartridge: Ninja Gaiden (USA), an iNES
 * image on an MMC1 (SUROM) board with 128 KiB of PRG ROM, 128 KiB of CHR ROM,
 * horizontal mirroring, no battery and no trainer.
 *
 * The check is deliberately strict.  A different dump of the same game (a
 * revision, a translation) still runs, but the launcher says so, because the
 * recompiled code is matched to the bytes of the reference dump.
 */
#include "launcher.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *const NGR_REFERENCE_SHA256 =
    "ce3245591dd63e535287851e5e7a6881d81f7f74c7e22152903fbd6e9a2585cd";

#define EXPECTED_MAPPER     1
#define EXPECTED_PRG_KIB   128
#define EXPECTED_CHR_KIB   128

static void set_summary(NgrRomInfo *out, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(out->summary, sizeof(out->summary), format, args);
    va_end(args);
}

bool ngr_validate_rom(const wchar_t *path, NgrRomInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->mapper = -1;
    out->submapper = -1;

    if (!path || !*path) {
        set_summary(out, "No ROM selected.");
        return false;
    }
    if (!ngr_file_exists(path)) {
        set_summary(out, "The file does not exist: %ls", path);
        return false;
    }

    unsigned char digest[32];
    if (!ngr_sha256_file(path, digest)) {
        set_summary(out, "The file could not be read.");
        return false;
    }
    ngr_hex(digest, out->sha256_hex, sizeof(out->sha256_hex));

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        set_summary(out, "The file could not be opened.");
        return false;
    }
    unsigned char header[16];
    DWORD read = 0;
    bool ok = ReadFile(file, header, 16, &read, NULL) && read == 16;
    LARGE_INTEGER size;
    GetFileSizeEx(file, &size);
    CloseHandle(file);
    if (!ok) {
        set_summary(out, "The file is too small to be an iNES image.");
        return false;
    }
    if (memcmp(header, "NES\x1a", 4) != 0) {
        set_summary(out, "Not an iNES image: the file does not start with the NES\\x1a header.");
        return false;
    }

    out->prg_kib = header[4] * 16;
    out->chr_kib = header[5] * 8;
    out->chr_is_ram = header[5] == 0;
    out->vertical_mirroring = (header[6] & 0x08) != 0;
    out->battery = (header[6] & 0x02) != 0;
    out->trainer = (header[6] & 0x04) != 0;
    out->four_screen = (header[6] & 0x08) != 0;
    out->nes2 = (header[7] & 0x04) != 0;
    out->mapper = (header[6] >> 4) | (header[7] & 0xF0);
    out->submapper = out->nes2 ? (header[7] & 0x0F) : 0;
    if (out->nes2) {
        /* NES 2.0: the mapper's high bits live in the seventh header byte. */
        out->mapper = (header[6] >> 4) | (header[7] & 0xF0);
    }

    snprintf(out->detail, sizeof(out->detail),
             "iNES header\n"
             "  mapper          %d%s\n"
             "  PRG ROM         %d KiB\n"
             "  CHR             %d KiB%s\n"
             "  mirroring       %s\n"
             "  battery         %s\n"
             "  trainer         %s\n"
             "  four-screen     %s\n"
             "  file size       %lld bytes\n"
             "  SHA-256         %s\n",
             out->mapper, out->nes2 ? " (NES 2.0)" : "", out->prg_kib, out->chr_kib,
             out->chr_is_ram ? " CHR RAM" : " CHR ROM",
             out->four_screen ? "four screen" : (out->vertical_mirroring ? "vertical" : "horizontal"),
             out->battery ? "yes" : "no", out->trainer ? "yes" : "no",
             out->four_screen ? "yes" : "no", (long long)size.QuadPart, out->sha256_hex);

    if (out->four_screen) {
        set_summary(out, "Four-screen VRAM cartridges are not supported by the NESRecomp runtime.");
        return false;
    }
    if (out->trainer) {
        set_summary(out, "The ROM has a trainer; this recompilation expects the plain iNES image.");
        return false;
    }
    if (out->mapper != EXPECTED_MAPPER) {
        set_summary(out, "Mapper %d is not supported by this recompilation (mapper %d, MMC1, is required).",
                    out->mapper, EXPECTED_MAPPER);
        return false;
    }
    if (out->prg_kib != EXPECTED_PRG_KIB) {
        set_summary(out, "PRG ROM is %d KiB; this recompilation expects %d KiB.",
                    out->prg_kib, EXPECTED_PRG_KIB);
        return false;
    }
    if (out->chr_is_ram || out->chr_kib != EXPECTED_CHR_KIB) {
        set_summary(out, "CHR is %d KiB%s; this recompilation expects %d KiB of CHR ROM.",
                    out->chr_kib, out->chr_is_ram ? " of RAM" : "", EXPECTED_CHR_KIB);
        return false;
    }
    if (out->battery) {
        set_summary(out, "The ROM declares battery RAM; the Ninja Gaiden (USA) board has none.");
        return false;
    }

    out->valid = true;
    out->exact_match = strcmp(out->sha256_hex, NGR_REFERENCE_SHA256) == 0;
    if (out->exact_match) {
        set_summary(out, "Ninja Gaiden (USA) - exact match (mapper %d, %d KiB PRG, %d KiB CHR, horizontal).",
                    out->mapper, out->prg_kib, out->chr_kib);
    } else {
        set_summary(out,
                    "Compatible cartridge (mapper %d, %d KiB PRG, %d KiB CHR) but not the reference dump: "
                    "this is another revision or a patch, and the game may not behave as expected.",
                    out->mapper, out->prg_kib, out->chr_kib);
    }
    return true;
}
