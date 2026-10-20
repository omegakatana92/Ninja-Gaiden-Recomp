/*
 * ui.c - the launcher's drawing helpers and small file utilities.
 */
#include "launcher.h"

#include <stdio.h>
#include <string.h>

const COLORREF NGR_BG     = RGB(0x14, 0x16, 0x1C);
const COLORREF NGR_PANEL  = RGB(0x1D, 0x20, 0x2A);
const COLORREF NGR_ACCENT = RGB(0xE0, 0x53, 0x1F);
const COLORREF NGR_TEXT   = RGB(0xEC, 0xEF, 0xF4);
const COLORREF NGR_MUTED  = RGB(0x8C, 0x93, 0xA3);
const COLORREF NGR_OK     = RGB(0x59, 0xC1, 0x7A);
const COLORREF NGR_WARN   = RGB(0xE8, 0xB3, 0x3C);
const COLORREF NGR_ERROR  = RGB(0xE0, 0x4F, 0x4F);

const char *const NGR_BUTTON_KEYS[8]   = { "a", "b", "select", "start", "up", "down", "left", "right" };
const char *const NGR_BUTTON_LABELS[8] = { "A", "B", "Select", "Start", "Up", "Down", "Left", "Right" };

void ngr_fill_round_rect(HDC hdc, RECT r, int radius, COLORREF fill, COLORREF border)
{
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ old = SelectObject(hdc, brush);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(hdc, old_pen);
    SelectObject(hdc, old);
    DeleteObject(pen);
    DeleteObject(brush);
}

void ngr_draw_text(HDC hdc, RECT r, const char *text, COLORREF colour, HFONT font, UINT flags)
{
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, colour);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, text, -1, &r, flags | DT_NOPREFIX);
    SelectObject(hdc, old);
}

HFONT ngr_font(int size, int weight, bool mono)
{
    return CreateFontA(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, mono ? FIXED_PITCH | FF_MODERN : DEFAULT_PITCH | FF_SWISS,
                       mono ? "Consolas" : "Segoe UI");
}

void ngr_join(wchar_t *out, size_t out_len, const wchar_t *a, const wchar_t *b)
{
    size_t len = wcslen(a);
    while (len && (a[len - 1] == L'\\' || a[len - 1] == L'/')) --len;
    size_t skip = 0;
    while (b[skip] == L'\\' || b[skip] == L'/') ++skip;
    _snwprintf_s(out, out_len, _TRUNCATE, L"%.*ls\\%ls", (int)len, a, b + skip);
}

void ngr_exe_dir(wchar_t *out, size_t out_len)
{
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)out_len);
    if (!n || n >= out_len) { out[0] = 0; return; }
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
    else out[0] = 0;
}

void ngr_appdata_dir(wchar_t *out, size_t out_len)
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) { out[0] = 0; return; }
    _snwprintf_s(out, out_len, _TRUNCATE, L"%ls\\NinjaGaidenRecomp", base);
}

bool ngr_file_exists(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool ngr_dir_exists(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool ngr_make_dirs(const wchar_t *path)
{
    if (!path || !*path) return false;
    if (ngr_dir_exists(path)) return true;
    wchar_t work[NGR_MAX_PATH];
    _snwprintf_s(work, NGR_MAX_PATH, _TRUNCATE, L"%ls", path);
    for (wchar_t *slash = wcschr(work, L'\\'); slash; slash = wcschr(slash + 1, L'\\')) {
        *slash = 0;
        if (work[0] && !(work[1] == 0 && work[2] == 0)) CreateDirectoryW(work, NULL);
        *slash = L'\\';
    }
    CreateDirectoryW(work, NULL);
    return ngr_dir_exists(path);
}

bool ngr_copy_file(const wchar_t *from, const wchar_t *to)
{
    return CopyFileW(from, to, FALSE) != 0;
}

bool ngr_write_text(const wchar_t *path, const char *text)
{
    FILE *file = NULL;
    if (_wfopen_s(&file, path, L"wb") != 0 || !file) return false;
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, file) == len;
    ok = fclose(file) == 0 && ok;
    return ok;
}

void ngr_wide(const char *text, wchar_t *out, size_t out_len)
{
    if (!out || !out_len) return;
    if (!text) { out[0] = 0; return; }
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, out, (int)out_len);
    if (n <= 0) { out[0] = 0; return; }
}

void ngr_narrow(const wchar_t *text, char *out, size_t out_len)
{
    if (!out || !out_len) return;
    if (!text) { out[0] = 0; return; }
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, (int)out_len, NULL, NULL);
    if (n <= 0) out[0] = 0;
}
