/*
 * launcher.c - the Ninja Gaiden recompilation launcher.
 *
 * The window offers what the project needs from a player:
 *   - pick the ROM they own and check that it is the cartridge this
 *     recompilation was made for,
 *   - build the native game from that ROM (recompile, compile, then record
 *     the routes the program takes so the rebuild compiles them natively),
 *   - play it, and
 *   - set how the game looks, sounds and is controlled.
 */
#include "launcher.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID_ROM_EDIT   100
#define ID_LOG_EDIT   101
#define ID_STATUS     102
#define ID_HASH       103
#define ID_FOOTER     104
#define ID_PROGRESS   105
#define ID_BUTTON0    200   /* Select ROM, Build, Play, Settings, Exit */
#define ID_BUTTON_COUNT 5
#define ID_TIMER      1
#define WM_BUILD_UPDATE (WM_APP + 1)

#define WIN_WIDTH  640
#define WIN_HEIGHT 580

typedef struct {
    HWND hwnd;
    HWND rom_edit;
    HWND log_edit;
    HWND status;
    HWND hash;
    HWND footer;
    HWND button[ID_BUTTON_COUNT];

    NgrRomInfo rom;
    wchar_t rom_path[NGR_MAX_PATH];
    wchar_t package_root[NGR_MAX_PATH];
    wchar_t engine_root[NGR_MAX_PATH];
    wchar_t recompiler[NGR_MAX_PATH];
    wchar_t route[NGR_MAX_PATH];
    wchar_t seed[NGR_MAX_PATH];
    wchar_t game_exe[NGR_MAX_PATH];
    wchar_t config_path[NGR_MAX_PATH];
    wchar_t launcher_config[NGR_MAX_PATH];

    NgrBuild *build;
    size_t   log_read;
    bool     building;
    bool     dirty;
    HFONT    font_title, font_sub, font_body, font_small, font_mono, font_button;
} Launcher;

static const char *const BUTTON_LABELS[ID_BUTTON_COUNT] = {
    "Select ROM...", "Build / Prepare Game", "Play Game", "Settings...", "Exit"
};

/* ------------------------------------------------------------- helpers --- */

static const char *button_label(int index)
{
    return (index >= 0 && index < ID_BUTTON_COUNT) ? BUTTON_LABELS[index] : "";
}

static void log_line(Launcher *app, const char *text)
{
    HWND log = app->log_edit;
    int length = GetWindowTextLengthW(log);
    SendMessageW(log, EM_SETSEL, (WPARAM)length, (LPARAM)length);
    wchar_t wide[2048];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 2048);
    SendMessageW(log, EM_REPLACESEL, FALSE, (LPARAM)wide);
    SendMessageW(log, EM_SCROLLCARET, 0, 0);
}

static void set_status(Launcher *app, const char *text, COLORREF colour)
{
    wchar_t wide[1024];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 1024);
    SetWindowTextW(app->status, wide);
    InvalidateRect(app->status, NULL, TRUE);
    /* the colour is carried by the control's text colour */
    SetPropW(app->status, L"colour", (HANDLE)(LONG_PTR)colour);
}

static void refresh_rom_display(Launcher *app)
{
    wchar_t text[NGR_MAX_PATH + 64];
    if (app->rom_path[0]) {
        _snwprintf_s(text, NGR_MAX_PATH + 64, _TRUNCATE, L"%ls", app->rom_path);
        SetWindowTextW(app->rom_edit, text);
    } else {
        SetWindowTextW(app->rom_edit, L"No ROM selected - choose your legally obtained copy of Ninja Gaiden (USA).");
    }
    if (app->rom.valid) {
        set_status(app, app->rom.summary, app->rom.exact_match ? NGR_OK : NGR_WARN);
        char hash_line[256];
        snprintf(hash_line, sizeof(hash_line), "SHA-256 %s", app->rom.sha256_hex);
        wchar_t wide[256];
        MultiByteToWideChar(CP_UTF8, 0, hash_line, -1, wide, 256);
        SetWindowTextW(app->hash, wide);
    } else {
        set_status(app, app->rom.summary, app->rom.summary[0] ? NGR_ERROR : NGR_MUTED);
        SetWindowTextW(app->hash, L"");
    }
    InvalidateRect(app->status, NULL, TRUE);
}

static void find_package_root(Launcher *app)
{
    wchar_t dir[NGR_MAX_PATH];
    ngr_exe_dir(dir, NGR_MAX_PATH);
    wchar_t probe[NGR_MAX_PATH];
    for (int level = 0; level < 6; ++level) {
        ngr_join(probe, NGR_MAX_PATH, dir, L"game.toml");
        wchar_t cmake[NGR_MAX_PATH];
        ngr_join(cmake, NGR_MAX_PATH, dir, L"CMakeLists.txt");
        if (ngr_file_exists(probe) && ngr_file_exists(cmake)) {
            _snwprintf_s(app->package_root, NGR_MAX_PATH, _TRUNCATE, L"%ls", dir);
            return;
        }
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (!slash || slash == dir) break;
        *slash = 0;
    }
    ngr_exe_dir(app->package_root, NGR_MAX_PATH);
}

static void locate_game(Launcher *app)
{
    static const wchar_t *const candidates[] = {
        L"out\\Release\\NinjaGaidenRecomp.exe",
        L"out\\NinjaGaidenRecomp.exe",
        L"build\\Release\\NinjaGaidenRecomp.exe",
        L"build\\NinjaGaidenRecomp.exe",
        L"NinjaGaidenRecomp.exe",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        wchar_t path[NGR_MAX_PATH];
        ngr_join(path, NGR_MAX_PATH, app->package_root, candidates[i]);
        if (ngr_file_exists(path)) {
            _snwprintf_s(app->game_exe, NGR_MAX_PATH, _TRUNCATE, L"%ls", path);
            wchar_t dir[NGR_MAX_PATH];
            _snwprintf_s(dir, NGR_MAX_PATH, _TRUNCATE, L"%ls", path);
            wchar_t *slash = wcsrchr(dir, L'\\');
            if (slash) *slash = 0;
            ngr_join(app->config_path, NGR_MAX_PATH, dir, L"config.ini");
            return;
        }
    }
    app->game_exe[0] = 0;
    ngr_join(app->config_path, NGR_MAX_PATH, app->package_root, L"out\\Release\\config.ini");
}

static void load_launcher_config(Launcher *app)
{
    NgrIni ini;
    ngr_ini_init(&ini);
    if (ngr_ini_load(&ini, app->launcher_config)) {
        const char *path = ngr_ini_get(&ini, "Launcher", "RomPath", "");
        if (path && path[0]) {
            wchar_t wide[NGR_MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, NGR_MAX_PATH);
            if (ngr_file_exists(wide)) {
                _snwprintf_s(app->rom_path, NGR_MAX_PATH, _TRUNCATE, L"%ls", wide);
                ngr_validate_rom(app->rom_path, &app->rom);
            }
        }
    }
    ngr_ini_free(&ini);
}

static void save_launcher_config(Launcher *app)
{
    wchar_t dir[NGR_MAX_PATH];
    ngr_appdata_dir(dir, NGR_MAX_PATH);
    if (dir[0]) CreateDirectoryW(dir, NULL);
    NgrIni ini;
    ngr_ini_init(&ini);
    ngr_ini_load(&ini, app->launcher_config);
    char narrow[NGR_MAX_PATH];
    ngr_narrow(app->rom_path, narrow, sizeof(narrow));
    ngr_ini_set(&ini, "Launcher", "RomPath", narrow);
    ngr_ini_save(&ini, app->launcher_config);
    ngr_ini_free(&ini);
}

/* ---------------------------------------------------------- the picker --- */

bool ngr_pick_rom(HWND owner, wchar_t *path, size_t path_len, const wchar_t *title)
{
    IFileOpenDialog *dialog = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IFileOpenDialog, (void **)&dialog);
    if (FAILED(hr)) return false;
    COMDLG_FILTERSPEC filters[2] = {
        { L"NES ROM images", L"*.nes" },
        { L"All files", L"*.*" },
    };
    dialog->lpVtbl->SetFileTypes(dialog, 2, filters);
    dialog->lpVtbl->SetTitle(dialog, title);
    DWORD options = 0;
    dialog->lpVtbl->GetOptions(dialog, &options);
    dialog->lpVtbl->SetOptions(dialog, options | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dialog->lpVtbl->Show(dialog, owner))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(dialog->lpVtbl->GetResult(dialog, &item))) {
            PWSTR name = NULL;
            if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &name))) {
                _snwprintf_s(path, path_len, _TRUNCATE, L"%ls", name);
                CoTaskMemFree(name);
                item->lpVtbl->Release(item);
                dialog->lpVtbl->Release(dialog);
                return true;
            }
            item->lpVtbl->Release(item);
        }
    }
    dialog->lpVtbl->Release(dialog);
    return false;
}

/* ------------------------------------------------------------- actions --- */

static void action_select_rom(Launcher *app)
{
    wchar_t path[NGR_MAX_PATH];
    _snwprintf_s(path, NGR_MAX_PATH, _TRUNCATE, L"%ls", app->rom_path);
    if (!ngr_pick_rom(app->hwnd, path, NGR_MAX_PATH, L"Select the Ninja Gaiden (USA) ROM")) return;

    NgrRomInfo info;
    ngr_validate_rom(path, &info);
    if (!info.valid) {
        wchar_t message[768];
        ngr_wide(info.summary, message, 768);
        MessageBoxW(app->hwnd, message, L"That ROM cannot be used",
                    MB_ICONWARNING | MB_OK);
        log_line(app, "The selected file was rejected:");
        log_line(app, info.detail);
        return;
    }
    _snwprintf_s(app->rom_path, NGR_MAX_PATH, _TRUNCATE, L"%ls", path);
    app->rom = info;
    refresh_rom_display(app);
    save_launcher_config(app);
    log_line(app, "ROM selected:");
    log_line(app, info.detail);
}

static void action_build(Launcher *app)
{
    if (app->building) return;
    if (!app->rom.valid) {
        MessageBoxW(app->hwnd, L"Select your legally obtained copy of Ninja Gaiden (USA) first.",
                    L"No ROM selected", MB_ICONINFORMATION | MB_OK);
        return;
    }
    locate_game(app);
    app->log_read = 0;
    SetWindowTextW(app->log_edit, L"");
    app->build = ngr_build_start(app->rom_path, app->package_root, app->recompiler,
                                 app->route, app->seed, app->hwnd, WM_BUILD_UPDATE);
    if (!app->build) {
        MessageBoxW(app->hwnd, L"The build thread could not be started.", L"Build failed",
                    MB_ICONERROR | MB_OK);
        return;
    }
    app->building = true;
    SetTimer(app->hwnd, ID_TIMER, 120, NULL);
    set_status(app, "Building...", NGR_ACCENT);
    log_line(app, "Building the native recompilation from the selected ROM.");
}

static void action_play(Launcher *app)
{
    if (!app->rom.valid) {
        MessageBoxW(app->hwnd, L"Select your legally obtained copy of Ninja Gaiden (USA) first.",
                    L"No ROM selected", MB_ICONINFORMATION | MB_OK);
        return;
    }
    locate_game(app);
    if (!app->game_exe[0] || !ngr_file_exists(app->game_exe)) {
        MessageBoxW(app->hwnd,
                    L"The game has not been built yet. Use \"Build / Prepare Game\" to build it from "
                    L"your ROM, then play.",
                    L"Nothing to play yet", MB_ICONINFORMATION | MB_OK);
        return;
    }
    wchar_t command[2 * NGR_MAX_PATH];
    _snwprintf_s(command, 2 * NGR_MAX_PATH, _TRUNCATE, L"\"%ls\" \"%ls\"", app->game_exe, app->rom_path);
    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));
    wchar_t dir[NGR_MAX_PATH];
    _snwprintf_s(dir, NGR_MAX_PATH, _TRUNCATE, L"%ls", app->game_exe);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    if (!CreateProcessW(app->game_exe, command, NULL, NULL, FALSE, 0, NULL, dir, &startup, &process)) {
        wchar_t message[512];
        _snwprintf_s(message, 512, _TRUNCATE,
                     L"The game could not be started (error %lu).", GetLastError());
        MessageBoxW(app->hwnd, message, L"Cannot start the game", MB_ICONERROR | MB_OK);
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    log_line(app, "The game was started.");
}

static void action_settings(Launcher *app)
{
    locate_game(app);
    wchar_t rom[NGR_MAX_PATH];
    _snwprintf_s(rom, NGR_MAX_PATH, _TRUNCATE, L"%ls", app->rom_path);
    if (ngr_settings_show(app->hwnd, app->config_path, rom, rom, NGR_MAX_PATH)) {
        _snwprintf_s(app->rom_path, NGR_MAX_PATH, _TRUNCATE, L"%ls", rom);
        if (app->rom_path[0]) {
            ngr_validate_rom(app->rom_path, &app->rom);
            refresh_rom_display(app);
            save_launcher_config(app);
        }
        log_line(app, "The settings were saved to config.ini.");
    }
}

/* ------------------------------------------------------------- drawing --- */

static void draw_panel(HDC hdc, RECT r, const char *title)
{
    ngr_fill_round_rect(hdc, r, 10, NGR_PANEL, RGB(0x2A, 0x2E, 0x3A));
    if (!title) return;
    HFONT font = ngr_font(9, FW_BOLD, false);
    RECT text = r;
    text.left += 14;
    text.top += 10;
    text.bottom = text.top + 18;
    ngr_draw_text(hdc, text, title, NGR_MUTED, font, DT_LEFT | DT_SINGLELINE);
    DeleteObject(font);
}

static void draw_progress(Launcher *app, HDC hdc, RECT r)
{
    ngr_fill_round_rect(hdc, r, 3, RGB(0x11, 0x13, 0x18), RGB(0x2A, 0x2E, 0x3A));
    if (!app->building) return;
    RECT inner = r;
    InflateRect(&inner, -1, -1);
    int width = inner.right - inner.left;
    int phase = (int)(GetTickCount() / 12) % (width + 120);
    RECT bar = inner;
    bar.left = inner.left + phase - 120;
    bar.right = bar.left + 120;
    if (bar.right > inner.right) bar.right = inner.right;
    if (bar.left < inner.left) bar.left = inner.left;
    ngr_fill_round_rect(hdc, bar, 3, NGR_ACCENT, NGR_ACCENT);
}

static LRESULT CALLBACK launcher_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    Launcher *app = (Launcher *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (message) {
        case WM_CREATE: {
            app = (Launcher *)((CREATESTRUCTW *)lparam)->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
            app->hwnd = hwnd;

            app->font_title = ngr_font(26, FW_BOLD, false);
            app->font_sub = ngr_font(9, FW_NORMAL, false);
            app->font_body = ngr_font(9, FW_NORMAL, false);
            app->font_small = ngr_font(8, FW_NORMAL, false);
            app->font_mono = ngr_font(8, FW_NORMAL, true);
            app->font_button = ngr_font(10, FW_SEMIBOLD, false);

            app->rom_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
                30, 128, 470, 24, hwnd, (HMENU)(INT_PTR)ID_ROM_EDIT, GetModuleHandleW(NULL), NULL);
            SendMessageW(app->rom_edit, WM_SETFONT, (WPARAM)app->font_body, TRUE);

            app->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                          30, 160, 580, 18, hwnd, (HMENU)(INT_PTR)ID_STATUS,
                                          GetModuleHandleW(NULL), NULL);
            SendMessageW(app->status, WM_SETFONT, (WPARAM)app->font_body, TRUE);

            app->hash = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                        30, 180, 580, 16, hwnd, (HMENU)(INT_PTR)ID_HASH,
                                        GetModuleHandleW(NULL), NULL);
            SendMessageW(app->hash, WM_SETFONT, (WPARAM)app->font_small, TRUE);

            app->log_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                30, 322, 580, 166, hwnd, (HMENU)(INT_PTR)ID_LOG_EDIT, GetModuleHandleW(NULL), NULL);
            SendMessageW(app->log_edit, WM_SETFONT, (WPARAM)app->font_mono, TRUE);
            SendMessageW(app->log_edit, EM_SETLIMITTEXT, 400000, 0);

            app->footer = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                          140, 506, 400, 20, hwnd, (HMENU)(INT_PTR)ID_FOOTER,
                                          GetModuleHandleW(NULL), NULL);
            SendMessageW(app->footer, WM_SETFONT, (WPARAM)app->font_small, TRUE);

            static const RECT button_rects[ID_BUTTON_COUNT] = {
                { 16, 232, 190, 272 },    /* Select ROM...          */
                { 198, 232, 424, 272 },   /* Build / Prepare Game   */
                { 432, 232, 542, 272 },   /* Play Game              */
                { 16, 500, 130, 530 },    /* Settings...            */
                { 550, 232, 624, 272 },   /* Exit                   */
            };
            for (int i = 0; i < ID_BUTTON_COUNT; ++i) {
                RECT r = button_rects[i];
                app->button[i] = CreateWindowExW(0, L"BUTTON", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                    r.left, r.top, r.right - r.left, r.bottom - r.top,
                    hwnd, (HMENU)(INT_PTR)(ID_BUTTON0 + i), GetModuleHandleW(NULL), NULL);
                SendMessageW(app->button[i], WM_SETFONT, (WPARAM)app->font_button, TRUE);
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            HBRUSH background = CreateSolidBrush(NGR_BG);
            FillRect(hdc, &client, background);
            DeleteObject(background);

            /* header */
            RECT header = { 0, 0, client.right, 86 };
            ngr_fill_round_rect(hdc, header, 0, RGB(0x1A, 0x1D, 0x26), RGB(0x1A, 0x1D, 0x26));
            RECT accent = { 0, 0, 6, 86 };
            ngr_fill_round_rect(hdc, accent, 0, NGR_ACCENT, NGR_ACCENT);
            RECT title = { 24, 16, 460, 50 };
            ngr_draw_text(hdc, title, "NINJA GAIDEN", NGR_TEXT, app->font_title, DT_LEFT | DT_SINGLELINE);
            RECT subtitle = { 26, 52, 520, 70 };
            ngr_draw_text(hdc, subtitle, NGR_SUBTITLE, NGR_MUTED, app->font_sub, DT_LEFT | DT_SINGLELINE);

            RECT badge = { client.right - 96, 22, client.right - 24, 62 };
            ngr_fill_round_rect(hdc, badge, 8, RGB(0x2A, 0x1A, 0x14), NGR_ACCENT);
            RECT badge_text = badge;
            ngr_draw_text(hdc, badge_text, "NES", NGR_ACCENT, app->font_title, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

            /* panels */
            RECT rom_panel = { 16, 98, client.right - 16, 222 };
            draw_panel(hdc, rom_panel, "GAME ROM");
            RECT log_panel = { 16, 296, client.right - 16, 500 };
            draw_panel(hdc, log_panel, "BUILD LOG");
            RECT progress = { 30, 278, client.right - 30, 284 };
            draw_progress(app, hdc, progress);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lparam;
            int index = item->CtlID - ID_BUTTON0;
            if (index < 0 || index >= ID_BUTTON_COUNT) break;
            HDC hdc = item->hDC;
            RECT r = item->rcItem;
            bool primary = (index == 1);
            bool pressed = (item->itemState & ODS_SELECTED) != 0;
            COLORREF fill = primary ? NGR_ACCENT : NGR_PANEL;
            COLORREF border = primary ? NGR_ACCENT : RGB(0x33, 0x38, 0x46);
            if (pressed) {
                fill = primary ? RGB(0xC2, 0x46, 0x18) : RGB(0x27, 0x2B, 0x36);
                InflateRect(&r, 1, 1);
                OffsetRect(&r, 0, 1);
            }
            ngr_fill_round_rect(hdc, r, 8, fill, border);
            RECT text = item->rcItem;
            ngr_draw_text(hdc, text, button_label(index), primary ? RGB(0x18, 0x10, 0x0C) : NGR_TEXT,
                          app->font_button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        case WM_COMMAND: {
            int id = LOWORD(wparam);
            if (id >= ID_BUTTON0 && id < ID_BUTTON0 + ID_BUTTON_COUNT) {
                switch (id - ID_BUTTON0) {
                    case 0: action_select_rom(app); break;
                    case 1: action_build(app); break;
                    case 2: action_play(app); break;
                    case 3: action_settings(app); break;
                    case 4: DestroyWindow(hwnd); break;
                    default: break;
                }
                return 0;
            }
            break;
        }
        case WM_TIMER: {
            if (app->build) {
                size_t length = ngr_build_output_length(app->build);
                if (length != app->log_read) {
                    char *copy = (char *)malloc(length + 1);
                    if (copy) {
                        memcpy(copy, ngr_build_output(app->build), length);
                        copy[length] = 0;
                        char *line = copy;
                        while (line && *line) {
                            char *end = strchr(line, '\n');
                            if (end) *end = 0;
                            if (*line) log_line(app, line);
                            line = end ? end + 1 : NULL;
                        }
                        free(copy);
                    }
                    app->log_read = length;
                }
                set_status(app, ngr_build_status(app->build), NGR_ACCENT);
                if (ngr_build_finished(app->build)) {
                    KillTimer(hwnd, ID_TIMER);
                    app->building = false;
                    if (ngr_build_succeeded(app->build)) {
                        set_status(app, "The game is ready - use \"Play Game\".", NGR_OK);
                        locate_game(app);
                    } else {
                        set_status(app, ngr_build_error(app->build), NGR_ERROR);
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_BUILD_UPDATE: {
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wparam;
            SetBkMode(dc, TRANSPARENT);
            HWND control = (HWND)lparam;
            int id = GetDlgCtrlID(control);
            if (id == ID_STATUS) {
                COLORREF colour = (COLORREF)(LONG_PTR)GetPropW(control, L"colour");
                SetTextColor(dc, colour ? colour : NGR_MUTED);
            } else {
                SetTextColor(dc, NGR_MUTED);
            }
            static HBRUSH brush;
            if (!brush) brush = CreateSolidBrush(NGR_BG);
            return (LRESULT)brush;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wparam;
            SetTextColor(dc, NGR_TEXT);
            SetBkColor(dc, RGB(0x16, 0x19, 0x20));
            static HBRUSH brush;
            if (!brush) brush = CreateSolidBrush(RGB(0x16, 0x19, 0x20));
            return (LRESULT)brush;
        }
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wparam;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, NGR_TEXT);
            static HBRUSH brush;
            if (!brush) brush = CreateSolidBrush(NGR_BG);
            return (LRESULT)brush;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            if (app->build) {
                ngr_build_stop(app->build);
                ngr_build_free(app->build);
                app->build = NULL;
            }
            save_launcher_config(app);
            DeleteObject(app->font_title);
            DeleteObject(app->font_sub);
            DeleteObject(app->font_body);
            DeleteObject(app->font_small);
            DeleteObject(app->font_mono);
            DeleteObject(app->font_button);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            PostQuitMessage(0);
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

/* ---------------------------------------------------------------- main --- */

/* A command line for the launcher itself, for tests and for scripts:
 *   --validate <rom>   check the ROM and print what was found
 *   --build <rom>      build the game from the ROM and print the log
 * The build is the same one the window runs. */

/* The launcher is a windowed program, so its output goes straight to the
 * handles it inherited: they work both for a console and for a pipe. */
static void emit(HANDLE handle, const char *text)
{
    if (handle == NULL || handle == INVALID_HANDLE_VALUE || !text || !*text) return;
    DWORD written = 0;
    WriteFile(handle, text, (DWORD)strlen(text), &written, NULL);
}

static int run_command_line(int argc, wchar_t **argv)
{
    int result = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--validate") && i + 1 < argc) {
            NgrRomInfo info;
            ngr_validate_rom(argv[++i], &info);
            emit(out, info.summary);
            emit(out, "\n");
            if (info.detail[0]) { emit(out, info.detail); emit(out, "\n"); }
            result = info.valid ? 0 : 1;
        } else if (!wcscmp(argv[i], L"--build") && i + 1 < argc) {
            Launcher app;
            memset(&app, 0, sizeof(app));
            find_package_root(&app);
            ngr_join(app.recompiler, NGR_MAX_PATH, app.package_root, L"tools\\NESRecomp.exe");
            ngr_join(app.route, NGR_MAX_PATH, app.package_root, L"docs\\route_play.txt");
            ngr_join(app.seed, NGR_MAX_PATH, app.package_root, L"seeds\\cyc_seeds.txt");
            _snwprintf_s(app.rom_path, NGR_MAX_PATH, _TRUNCATE, L"%ls", argv[++i]);
            NgrRomInfo info;
            ngr_validate_rom(app.rom_path, &info);
            if (!info.valid) {
                emit(err, info.summary);
                emit(err, "\n");
                return 1;
            }
            app.build = ngr_build_start(app.rom_path, app.package_root, app.recompiler,
                                        app.route, app.seed, NULL, 0);
            if (!app.build) {
                emit(err, "The build thread could not be started.\n");
                return 1;
            }
            size_t shown = 0;
            while (!ngr_build_finished(app.build)) {
                size_t length = ngr_build_output_length(app.build);
                if (length > shown) {
                    const char *text = ngr_build_output(app.build);
                    emit(out, text + shown);
                    shown = length;
                }
                Sleep(100);
            }
            size_t length = ngr_build_output_length(app.build);
            if (length > shown) {
                const char *text = ngr_build_output(app.build);
                emit(out, text + shown);
                shown = length;
            }
            (void)shown;
            if (ngr_build_succeeded(app.build)) {
                emit(out, "BUILD OK\n");
                result = 0;
            } else {
                emit(err, "BUILD FAILED: ");
                emit(err, ngr_build_error(app.build));
                emit(err, "\n");
                result = 1;
            }
            ngr_build_free(app.build);
        }
    }
    return result;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show)
{
    (void)previous;
    (void)command_line;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int command_result = 0;
    bool has_command = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--validate") || !wcscmp(argv[i], L"--build")) { has_command = true; break; }
    }
    if (has_command) {
        command_result = run_command_line(argc, argv);
        LocalFree(argv);
        return command_result;
    }
    LocalFree(argv);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls;
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    Launcher app;
    memset(&app, 0, sizeof(app));

    find_package_root(&app);
    ngr_join(app.recompiler, NGR_MAX_PATH, app.package_root, L"tools\\NESRecomp.exe");
    ngr_join(app.route, NGR_MAX_PATH, app.package_root, L"docs\\route_play.txt");
    ngr_join(app.seed, NGR_MAX_PATH, app.package_root, L"seeds\\cyc_seeds.txt");
    wchar_t appdata[NGR_MAX_PATH];
    ngr_appdata_dir(appdata, NGR_MAX_PATH);
    ngr_join(app.launcher_config, NGR_MAX_PATH, appdata, L"launcher.ini");
    locate_game(&app);
    load_launcher_config(&app);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = launcher_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(INT_PTR)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"NinjaGaidenLauncher";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"NinjaGaidenLauncher", L"Ninja Gaiden - Native Recompilation",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, WIN_WIDTH, WIN_HEIGHT,
                                NULL, NULL, instance, &app);
    if (!hwnd) {
        CoUninitialize();
        return 1;
    }

    /* centre the window */
    RECT rect;
    GetWindowRect(hwnd, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd, NULL, (screen_width - width) / 2, (screen_height - height) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    refresh_rom_display(&app);

    wchar_t footer[512];
    _snwprintf_s(footer, 512, _TRUNCATE, L"Package: %ls", app.package_root);
    SetWindowTextW(app.footer, footer);
    log_line(&app, "Ninja Gaiden (USA) native recompilation launcher.");
    log_line(&app, "Select your ROM, then use \"Build / Prepare Game\" to build it and \"Play Game\" to play.");
    if (app.rom.valid) {
        log_line(&app, "The last ROM was restored:");
        log_line(&app, app.rom.detail);
    }

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return 0;
}
