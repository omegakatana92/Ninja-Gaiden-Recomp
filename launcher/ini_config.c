/*
 * ini_config.c - read and write the game's config.ini.
 *
 * The cycle backend keeps its settings next to the executable, in config.ini
 * ([Display], [Audio], [Input], [Keyboard.PlayerN], [Gamepad.PlayerN], ...).
 * The launcher edits the same file so its settings and the game's own menu
 * always agree.  Unknown sections and keys are preserved.
 */
#include "launcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ngr_ini_init(NgrIni *ini)
{
    ini->entries = NULL;
    ini->count = 0;
    ini->capacity = 0;
}

void ngr_ini_free(NgrIni *ini)
{
    free(ini->entries);
    ngr_ini_init(ini);
}

static bool ini_reserve(NgrIni *ini, size_t needed)
{
    if (needed <= ini->capacity) return true;
    size_t capacity = ini->capacity ? ini->capacity * 2 : 32;
    while (capacity < needed) capacity *= 2;
    NgrIniEntry *grown = (NgrIniEntry *)realloc(ini->entries, capacity * sizeof(*grown));
    if (!grown) return false;
    ini->entries = grown;
    ini->capacity = capacity;
    return true;
}

static void trim(char *text)
{
    size_t len = strlen(text);
    while (len && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\r' || text[len - 1] == '\n'))
        text[--len] = 0;
    size_t start = 0;
    while (text[start] == ' ' || text[start] == '\t') ++start;
    if (start) memmove(text, text + start, len - start + 1);
}

bool ngr_ini_load(NgrIni *ini, const wchar_t *path)
{
    FILE *file = NULL;
    if (_wfopen_s(&file, path, L"r") != 0 || !file) return false;
    char section[64] = "";
    char line[512];
    while (fgets(line, (int)sizeof(line), file)) {
        char *comment = strchr(line, ';');
        if (comment) *comment = 0;
        comment = strchr(line, '#');
        if (comment) *comment = 0;
        trim(line);
        if (!line[0]) continue;
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end) continue;
            *end = 0;
            snprintf(section, sizeof(section), "%s", line + 1);
            trim(section);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char key[64], value[256];
        snprintf(key, sizeof(key), "%s", line);
        snprintf(value, sizeof(value), "%s", eq + 1);
        trim(key);
        trim(value);
        if (!key[0]) continue;
        if (!ini_reserve(ini, ini->count + 1)) { fclose(file); return false; }
        NgrIniEntry *entry = &ini->entries[ini->count++];
        snprintf(entry->section, sizeof(entry->section), "%s", section);
        snprintf(entry->key, sizeof(entry->key), "%s", key);
        snprintf(entry->value, sizeof(entry->value), "%s", value);
    }
    fclose(file);
    return true;
}

bool ngr_ini_save(const NgrIni *ini, const wchar_t *path)
{
    wchar_t temp[NGR_MAX_PATH];
    _snwprintf_s(temp, NGR_MAX_PATH, _TRUNCATE, L"%ls.tmp", path);
    FILE *file = NULL;
    if (_wfopen_s(&file, temp, L"w") != 0 || !file) return false;
    fprintf(file, "# NESRecomp settings, edited by the Ninja Gaiden launcher.\n");
    char section[64] = "";
    for (size_t i = 0; i < ini->count; ++i) {
        const NgrIniEntry *entry = &ini->entries[i];
        if (strcmp(section, entry->section) != 0) {
            snprintf(section, sizeof(section), "%s", entry->section);
            fprintf(file, "[%s]\n", section);
        }
        fprintf(file, "%s = %s\n", entry->key, entry->value);
    }
    bool ok = fflush(file) == 0;
    ok = fclose(file) == 0 && ok;
    if (!ok) { DeleteFileW(temp); return false; }
    if (!MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp);
        return false;
    }
    return true;
}

const char *ngr_ini_get(const NgrIni *ini, const char *section, const char *key, const char *fallback)
{
    for (size_t i = 0; i < ini->count; ++i) {
        if (_stricmp(ini->entries[i].section, section) == 0 && _stricmp(ini->entries[i].key, key) == 0)
            return ini->entries[i].value;
    }
    return fallback;
}

int ngr_ini_get_int(const NgrIni *ini, const char *section, const char *key, int fallback)
{
    const char *text = ngr_ini_get(ini, section, key, NULL);
    if (!text || !*text) return fallback;
    return atoi(text);
}

void ngr_ini_set(NgrIni *ini, const char *section, const char *key, const char *value)
{
    for (size_t i = 0; i < ini->count; ++i) {
        if (_stricmp(ini->entries[i].section, section) == 0 && _stricmp(ini->entries[i].key, key) == 0) {
            snprintf(ini->entries[i].value, sizeof(ini->entries[i].value), "%s", value);
            return;
        }
    }
    if (!ini_reserve(ini, ini->count + 1)) return;
    NgrIniEntry *entry = &ini->entries[ini->count++];
    snprintf(entry->section, sizeof(entry->section), "%s", section);
    snprintf(entry->key, sizeof(entry->key), "%s", key);
    snprintf(entry->value, sizeof(entry->value), "%s", value);
}

void ngr_ini_set_int(NgrIni *ini, const char *section, const char *key, int value)
{
    char text[32];
    snprintf(text, sizeof(text), "%d", value);
    ngr_ini_set(ini, section, key, text);
}
