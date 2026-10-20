/*
 * launcher.h - shared declarations for the Ninja Gaiden recompilation launcher.
 *
 * The launcher is a small Win32 program with three jobs:
 *   - pick and validate the player's own legally obtained ROM,
 *   - build the native recompilation locally from that ROM,
 *   - launch the game and configure how it looks and feels.
 *
 * Everything it writes lives beside the game executable (config.ini) or in the
 * user's profile (%APPDATA%\NinjaGaidenRecomp\launcher.ini).
 */
#ifndef NGR_LAUNCHER_H
#define NGR_LAUNCHER_H

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>

#define NGR_MAX_PATH 1024
#define NGR_TITLE     "Ninja Gaiden (USA)"
#define NGR_SUBTITLE  "Native Windows x64 recompilation - NESRecomp, cycle-accurate backend"

/* ------------------------------------------------------------------ ROM --- */

typedef struct {
    bool valid;              /* an iNES file the header parser accepted      */
    bool exact_match;        /* the byte-identical Ninja Gaiden (USA) dump   */
    int  mapper;
    int  submapper;
    int  prg_kib;            /* PRG ROM size in KiB                           */
    int  chr_kib;            /* CHR ROM (or CHR RAM) size in KiB              */
    bool chr_is_ram;
    bool battery;
    bool trainer;
    bool four_screen;
    bool vertical_mirroring;
    bool nes2;
    char sha256_hex[65];
    char summary[512];       /* one line for the UI                          */
    char detail[1024];       /* the full report for the log                  */
} NgrRomInfo;

/* Reads and checks the header.  Returns false when the file cannot be used;
 * out->summary then holds the reason in plain ASCII. */
bool ngr_validate_rom(const wchar_t *path, NgrRomInfo *out);

/* The digest of the reference Ninja Gaiden (USA) dump this project targets. */
extern const char *const NGR_REFERENCE_SHA256;

/* --------------------------------------------------------------- SHA-256 --- */

void ngr_sha256(const void *data, size_t len, unsigned char digest[32]);
bool ngr_sha256_file(const wchar_t *path, unsigned char digest[32]);
void ngr_hex(const unsigned char *digest, char *out, size_t out_len);

/* ------------------------------------------------------------- INI file --- */

/* A tiny INI reader/writer for the game's config.ini.  The game writes the
 * same file, so the launcher edits keys in place and keeps the rest. */
typedef struct {
    char section[64];
    char key[64];
    char value[256];
} NgrIniEntry;

typedef struct {
    NgrIniEntry *entries;
    size_t       count;
    size_t       capacity;
} NgrIni;

void        ngr_ini_init(NgrIni *ini);
void        ngr_ini_free(NgrIni *ini);
bool        ngr_ini_load(NgrIni *ini, const wchar_t *path);
bool        ngr_ini_save(const NgrIni *ini, const wchar_t *path);
const char *ngr_ini_get(const NgrIni *ini, const char *section, const char *key, const char *fallback);
int         ngr_ini_get_int(const NgrIni *ini, const char *section, const char *key, int fallback);
void        ngr_ini_set(NgrIni *ini, const char *section, const char *key, const char *value);
void        ngr_ini_set_int(NgrIni *ini, const char *section, const char *key, int value);

/* --------------------------------------------------------- build runner --- */

/* The local build: recompile the ROM to C, compile it, and - when the seed
 * file is still empty - play a recorded route to collect the addresses the
 * interpreter reached, then rebuild so the game runs its own code. */
typedef struct NgrBuild NgrBuild;

/* msg is posted to hwnd whenever a new line of output is available. */
NgrBuild *ngr_build_start(const wchar_t *rom_path, const wchar_t *package_root,
                           const wchar_t *recompiler, const wchar_t *route_path,
                           const wchar_t *seed_path, HWND hwnd, UINT msg);
void      ngr_build_stop(NgrBuild *build);
void      ngr_build_free(NgrBuild *build);
bool      ngr_build_finished(NgrBuild *build);
bool      ngr_build_succeeded(NgrBuild *build);
const char *ngr_build_status(NgrBuild *build);
const char *ngr_build_error(NgrBuild *build);
const char *ngr_build_output(NgrBuild *build);   /* everything printed so far */
size_t    ngr_build_output_length(NgrBuild *build);

/* ----------------------------------------------------------- UI helpers --- */

extern const COLORREF NGR_BG;
extern const COLORREF NGR_PANEL;
extern const COLORREF NGR_ACCENT;
extern const COLORREF NGR_TEXT;
extern const COLORREF NGR_MUTED;
extern const COLORREF NGR_OK;
extern const COLORREF NGR_WARN;
extern const COLORREF NGR_ERROR;

void        ngr_fill_round_rect(HDC hdc, RECT r, int radius, COLORREF fill, COLORREF border);
void        ngr_draw_text(HDC hdc, RECT r, const char *text, COLORREF colour, HFONT font, UINT flags);
HFONT       ngr_font(int size, int weight, bool mono);
void        ngr_join(wchar_t *out, size_t out_len, const wchar_t *a, const wchar_t *b);
bool        ngr_copy_file(const wchar_t *from, const wchar_t *to);
bool        ngr_write_text(const wchar_t *path, const char *text);
void        ngr_wide(const char *text, wchar_t *out, size_t out_len);
void        ngr_narrow(const wchar_t *text, char *out, size_t out_len);
void        ngr_exe_dir(wchar_t *out, size_t out_len);
void        ngr_appdata_dir(wchar_t *out, size_t out_len);
bool        ngr_file_exists(const wchar_t *path);
bool        ngr_dir_exists(const wchar_t *path);
bool        ngr_make_dirs(const wchar_t *path);
void        ngr_join(wchar_t *out, size_t out_len, const wchar_t *a, const wchar_t *b);
bool        ngr_copy_file(const wchar_t *from, const wchar_t *to);
bool        ngr_write_text(const wchar_t *path, const char *text);

/* The keys the game reads, in the order config.ini lists them. */
extern const char *const NGR_BUTTON_KEYS[8];
extern const char *const NGR_BUTTON_LABELS[8];

/* ------------------------------------------------------------- launcher --- */

/* The standard Windows file picker, filtered to .nes files. */
bool ngr_pick_rom(HWND owner, wchar_t *path, size_t path_len, const wchar_t *title);

/* The settings window.  Returns true when the player saved; the ROM path is
 * written back to rom_out so a change made there is kept. */
bool ngr_settings_show(HWND owner, const wchar_t *config_path, const wchar_t *rom_path,
                        wchar_t *rom_out, size_t rom_out_len);

#endif /* NGR_LAUNCHER_H */
