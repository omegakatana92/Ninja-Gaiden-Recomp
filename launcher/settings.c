/*
 * settings.c - the launcher's settings window.
 *
 * The game keeps its settings in config.ini next to its executable; this
 * window edits the same file, so the launcher and the game always agree.
 * The keyboard and controller names are the ones the game expects (SDL's own
 * names), which is why the window uses SDL for the name tables and for
 * reading a controller while a binding is being recorded.
 */
#include "launcher.h"

#include <SDL.h>
#include <ctype.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID_STATIC  100
#define ID_COMBO   101
#define ID_CHECK   102
#define ID_BUTTON  103
#define ID_TRACK   104
#define ID_KEYCAP  105

#define ID_OK      200
#define ID_CANCEL  201
#define ID_CLEAR   202
#define ID_BROWSE  203

#define CAPTURE_NONE (-1)

typedef struct {
    HWND hwnd;
    HWND fullscreen;
    HWND scale;
    HWND integer_scale;
    HWND linear_filter;
    HWND vsync;
    HWND audio_enabled;
    HWND volume;
    HWND volume_label;
    HWND source;
    HWND key_button[8];
    HWND pad_button[8];
    HWND rom_label;
    HWND status;

    NgrIni ini;
    int  capture_key;
    int  capture_pad;
    int  timer;
    bool ok;
    bool sdl_ready;
    wchar_t rom[NGR_MAX_PATH];
} SettingsWindow;

/* ------------------------------------------------------- key translation --- */

/* The Win32 virtual key of a key press -> the SDL scancode the game stores. */
static int vk_to_scancode(WPARAM vk, LPARAM lparam)
{
    switch (vk) {
        case VK_LEFT:   return SDL_SCANCODE_LEFT;
        case VK_RIGHT:  return SDL_SCANCODE_RIGHT;
        case VK_UP:     return SDL_SCANCODE_UP;
        case VK_DOWN:   return SDL_SCANCODE_DOWN;
        case VK_RETURN: return SDL_SCANCODE_RETURN;
        case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
        case VK_BACK:   return SDL_SCANCODE_BACKSPACE;
        case VK_TAB:    return SDL_SCANCODE_TAB;
        case VK_SPACE:  return SDL_SCANCODE_SPACE;
        case VK_PRIOR:  return SDL_SCANCODE_PAGEUP;
        case VK_NEXT:   return SDL_SCANCODE_PAGEDOWN;
        case VK_HOME:   return SDL_SCANCODE_HOME;
        case VK_END:    return SDL_SCANCODE_END;
        case VK_INSERT: return SDL_SCANCODE_INSERT;
        case VK_DELETE: return SDL_SCANCODE_DELETE;
        case VK_SHIFT:
            return (lparam & 0x01000000) ? SDL_SCANCODE_RSHIFT : SDL_SCANCODE_LSHIFT;
        case VK_CONTROL:
            return (lparam & 0x01000000) ? SDL_SCANCODE_RCTRL : SDL_SCANCODE_LCTRL;
        case VK_MENU:
            return (lparam & 0x01000000) ? SDL_SCANCODE_RALT : SDL_SCANCODE_LALT;
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) return SDL_SCANCODE_F1 + (int)(vk - VK_F1);
    if (vk >= 'A' && vk <= 'Z') return SDL_SCANCODE_A + (int)(vk - 'A');
    if (vk >= '1' && vk <= '9') return SDL_SCANCODE_1 + (int)(vk - '1');
    if (vk == '0') return SDL_SCANCODE_0;
    switch (vk) {
        case VK_OEM_MINUS:  return SDL_SCANCODE_MINUS;
        case VK_OEM_PLUS:   return SDL_SCANCODE_EQUALS;
        case VK_OEM_4:      return SDL_SCANCODE_LEFTBRACKET;
        case VK_OEM_6:      return SDL_SCANCODE_RIGHTBRACKET;
        case VK_OEM_5:      return SDL_SCANCODE_BACKSLASH;
        case VK_OEM_1:      return SDL_SCANCODE_SEMICOLON;
        case VK_OEM_7:      return SDL_SCANCODE_APOSTROPHE;
        case VK_OEM_3:      return SDL_SCANCODE_GRAVE;
        case VK_OEM_COMMA:  return SDL_SCANCODE_COMMA;
        case VK_OEM_PERIOD: return SDL_SCANCODE_PERIOD;
        case VK_OEM_2:      return SDL_SCANCODE_SLASH;
        default: break;
    }
    return SDL_SCANCODE_UNKNOWN;
}

/* ------------------------------------------------------------- helpers --- */

static void set_font(HWND control, HFONT font)
{
    SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
}

static HWND add_control(HWND parent, const wchar_t *class_name, const wchar_t *text, DWORD style,
                        int x, int y, int width, int height, int id, HFONT font)
{
    HWND control = CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style,
                                   x, y, width, height, parent, (HMENU)(INT_PTR)id,
                                   GetModuleHandleW(NULL), NULL);
    if (control && font) set_font(control, font);
    return control;
}

static void fill_combo(HWND combo, const char *const *items, int count, int selected)
{
    for (int i = 0; i < count; ++i) {
        wchar_t wide[64];
        MultiByteToWideChar(CP_UTF8, 0, items[i], -1, wide, 64);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)wide);
    }
    SendMessageW(combo, CB_SETCURSEL, (WPARAM)selected, 0);
}

static int combo_selection(HWND combo)
{
    return (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
}

static void update_key_labels(SettingsWindow *win)
{
    for (int i = 0; i < 8; ++i) {
        const char *text = ngr_ini_get(&win->ini, "Keyboard.Player1", NGR_BUTTON_KEYS[i], "None");
        wchar_t wide[64];
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 64);
        SetWindowTextW(win->key_button[i], wide);
    }
    for (int i = 0; i < 8; ++i) {
        const char *text = ngr_ini_get(&win->ini, "Gamepad.Player1", NGR_BUTTON_KEYS[i], "none");
        wchar_t wide[64];
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 64);
        SetWindowTextW(win->pad_button[i], wide);
    }
}

static void update_volume_label(SettingsWindow *win)
{
    int volume = (int)SendMessageW(win->volume, TBM_GETPOS, 0, 0);
    wchar_t text[32];
    _snwprintf_s(text, 32, _TRUNCATE, L"%d%%", volume);
    SetWindowTextW(win->volume_label, text);
}

/* ------------------------------------------------------------- layout --- */

#define DLG_WIDTH  620
#define DLG_HEIGHT 560

static void create_controls(SettingsWindow *win, HFONT font_body, HFONT font_bold, HFONT font_small)
{
    static const char *const FULLSCREEN[] = { "Off", "Borderless", "Exclusive fullscreen" };
    static const char *const SCALES[] = { "1x", "2x", "3x", "4x", "5x", "6x", "7x", "8x" };
    static const char *const SOURCES[] = { "Keyboard", "Gamepad", "None" };

    HWND parent = win->hwnd;
    RECT client;
    GetClientRect(parent, &client);

    HWND title = add_control(parent, L"STATIC", L"Settings", WS_CHILD | WS_VISIBLE, 18, 12, 300, 26,
                             ID_STATIC, font_bold);
    set_font(title, font_bold);
    add_control(parent, L"STATIC",
                L"These are the game's own settings; they are stored in config.ini next to the game.",
                WS_CHILD | WS_VISIBLE, 18, 36, DLG_WIDTH - 36, 18, ID_STATIC, font_small);

    /* Display */
    add_control(parent, L"GROUPBOX", L"Display", WS_CHILD | WS_VISIBLE, 14, 62, 300, 132, ID_STATIC, font_body);
    add_control(parent, L"STATIC", L"Fullscreen:", WS_CHILD | WS_VISIBLE, 26, 84, 90, 20, ID_STATIC, font_body);
    win->fullscreen = add_control(parent, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                  120, 80, 180, 200, ID_COMBO, font_body);
    fill_combo(win->fullscreen, FULLSCREEN, 3, 0);

    add_control(parent, L"STATIC", L"Window scale:", WS_CHILD | WS_VISIBLE, 26, 110, 90, 20, ID_STATIC, font_body);
    win->scale = add_control(parent, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                              120, 106, 180, 200, ID_COMBO, font_body);
    fill_combo(win->scale, SCALES, 8, 2);

    win->integer_scale = add_control(parent, L"BUTTON", L"Integer scaling", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     26, 136, 140, 22, ID_CHECK, font_body);
    win->linear_filter = add_control(parent, L"BUTTON", L"Linear filtering", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     26, 160, 140, 22, ID_CHECK, font_body);
    win->vsync = add_control(parent, L"BUTTON", L"VSync", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                             170, 160, 100, 22, ID_CHECK, font_body);

    /* Audio */
    add_control(parent, L"GROUPBOX", L"Audio", WS_CHILD | WS_VISIBLE, 14, 200, 300, 104, ID_STATIC, font_body);
    win->audio_enabled = add_control(parent, L"BUTTON", L"Sound enabled", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     26, 222, 140, 22, ID_CHECK, font_body);
    add_control(parent, L"STATIC", L"Volume:", WS_CHILD | WS_VISIBLE, 26, 256, 60, 20, ID_STATIC, font_body);
    win->volume = add_control(parent, L"TRACKBAR32", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
                              90, 254, 190, 30, ID_TRACK, NULL);
    SendMessageW(win->volume, TBM_SETRANGE, TRUE, (LPARAM)MAKELONG(0, 100));
    win->volume_label = add_control(parent, L"STATIC", L"100%", WS_CHILD | WS_VISIBLE, 284, 256, 40, 20, ID_STATIC, font_body);

    /* Controls */
    add_control(parent, L"GROUPBOX", L"Controls - player 1", WS_CHILD | WS_VISIBLE, 14, 310, 592, 150, ID_STATIC, font_body);
    add_control(parent, L"STATIC", L"Input source:", WS_CHILD | WS_VISIBLE, 26, 330, 90, 20, ID_STATIC, font_body);
    win->source = add_control(parent, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                              120, 326, 140, 200, ID_COMBO, font_body);
    fill_combo(win->source, SOURCES, 3, 0);

    add_control(parent, L"STATIC", L"Keyboard:", WS_CHILD | WS_VISIBLE, 26, 360, 80, 20, ID_STATIC, font_body);
    add_control(parent, L"STATIC", L"Gamepad:", WS_CHILD | WS_VISIBLE, 26, 400, 80, 20, ID_STATIC, font_body);
    for (int i = 0; i < 8; ++i) {
        wchar_t label[32];
        _snwprintf_s(label, 32, _TRUNCATE, L"%hs", NGR_BUTTON_LABELS[i]);
        int x = 110 + i * 60;
        win->key_button[i] = add_control(parent, L"BUTTON", label,
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                         x, 356, 54, 24, ID_KEYCAP, font_body);
        win->pad_button[i] = add_control(parent, L"BUTTON", label,
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                         x, 396, 54, 24, ID_KEYCAP, font_body);
    }

    /* ROM */
    add_control(parent, L"GROUPBOX", L"Game ROM", WS_CHILD | WS_VISIBLE, 14, 466, 592, 58, ID_STATIC, font_body);
    win->rom_label = add_control(parent, L"STATIC", L"(none)", WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS,
                                 26, 486, 420, 20, ID_STATIC, font_body);
    add_control(parent, L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 456, 482, 80, 24, ID_BROWSE, font_body);
    add_control(parent, L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 542, 482, 56, 24, ID_CLEAR, font_body);

    win->status = add_control(parent, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 18, 528, 580, 18, ID_STATIC, font_small);

    HWND ok = add_control(parent, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                          430, 528, 84, 26, ID_OK, font_body);
    add_control(parent, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 520, 528, 84, 26, ID_CANCEL, font_body);
    SetFocus(ok);
}

/* ---------------------------------------------------------- the dialog --- */

static void load_settings(SettingsWindow *win)
{
    int fullscreen = ngr_ini_get_int(&win->ini, "Display", "Fullscreen", 0);
    SendMessageW(win->fullscreen, CB_SETCURSEL, (WPARAM)(fullscreen < 0 ? 0 : (fullscreen > 2 ? 2 : fullscreen)), 0);
    int scale = ngr_ini_get_int(&win->ini, "Display", "WindowScale", 3);
    SendMessageW(win->scale, CB_SETCURSEL, (WPARAM)(scale < 1 ? 0 : (scale > 8 ? 7 : scale - 1)), 0);
    CheckDlgButton(win->hwnd, (UINT)GetDlgCtrlID(win->integer_scale),
                   ngr_ini_get_int(&win->ini, "Display", "IntegerScale", 0) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(win->hwnd, (UINT)GetDlgCtrlID(win->linear_filter),
                   ngr_ini_get_int(&win->ini, "Display", "LinearFilter", 0) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(win->hwnd, (UINT)GetDlgCtrlID(win->vsync),
                   ngr_ini_get_int(&win->ini, "Display", "VSync", 0) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(win->hwnd, (UINT)GetDlgCtrlID(win->audio_enabled),
                   ngr_ini_get_int(&win->ini, "Audio", "Enabled", 1) ? BST_CHECKED : BST_UNCHECKED);
    int volume = ngr_ini_get_int(&win->ini, "Audio", "Volume", 100);
    SendMessageW(win->volume, TBM_SETPOS, TRUE, (LPARAM)(volume < 0 ? 0 : (volume > 100 ? 100 : volume)));
    int source = ngr_ini_get_int(&win->ini, "Input", "Player1Source", 1);
    SendMessageW(win->source, CB_SETCURSEL, (WPARAM)(source < 0 ? 0 : (source > 2 ? 2 : source)), 0);
    update_key_labels(win);
    update_volume_label(win);
    wchar_t rom[512];
    _snwprintf_s(rom, 512, _TRUNCATE, L"%ls", win->rom);
    SetWindowTextW(win->rom_label, rom[0] ? rom : L"(none)");
}

static void store_settings(SettingsWindow *win)
{
    ngr_ini_set_int(&win->ini, "Display", "Fullscreen", combo_selection(win->fullscreen));
    ngr_ini_set_int(&win->ini, "Display", "WindowScale", combo_selection(win->scale) + 1);
    ngr_ini_set_int(&win->ini, "Display", "IntegerScale",
                    SendDlgItemMessageW(win->hwnd, (UINT)GetDlgCtrlID(win->integer_scale), BM_GETCHECK, 0, 0) == BST_CHECKED);
    ngr_ini_set_int(&win->ini, "Display", "LinearFilter",
                    SendDlgItemMessageW(win->hwnd, (UINT)GetDlgCtrlID(win->linear_filter), BM_GETCHECK, 0, 0) == BST_CHECKED);
    ngr_ini_set_int(&win->ini, "Display", "VSync",
                    SendDlgItemMessageW(win->hwnd, (UINT)GetDlgCtrlID(win->vsync), BM_GETCHECK, 0, 0) == BST_CHECKED);
    ngr_ini_set_int(&win->ini, "Audio", "Enabled",
                    SendDlgItemMessageW(win->hwnd, (UINT)GetDlgCtrlID(win->audio_enabled), BM_GETCHECK, 0, 0) == BST_CHECKED);
    ngr_ini_set_int(&win->ini, "Audio", "Volume", (int)SendMessageW(win->volume, TBM_GETPOS, 0, 0));
    ngr_ini_set_int(&win->ini, "Input", "Player1Source", combo_selection(win->source));
}

/* Reads a controller press while a gamepad binding is being recorded. */
static void poll_gamepad(SettingsWindow *win)
{
    if (win->capture_pad < 0) return;
    if (!win->sdl_ready) return;
    int count = SDL_NumJoysticks();
    for (int i = 0; i < count; ++i) {
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController *pad = SDL_GameControllerOpen(i);
        if (!pad) continue;
        for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button) {
            if (!SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)button)) continue;
            const char *name = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)button);
            if (!name) continue;
            char lower[64];
            snprintf(lower, sizeof(lower), "%s", name);
            for (char *c = lower; *c; ++c) *c = (char)tolower((unsigned char)*c);
            int slot = win->capture_pad;
            ngr_ini_set(&win->ini, "Gamepad.Player1", NGR_BUTTON_KEYS[slot], lower);
            win->capture_pad = CAPTURE_NONE;
            update_key_labels(win);
            SetWindowTextW(win->status, L"Gamepad button recorded.");
            SDL_GameControllerClose(pad);
            return;
        }
        SDL_GameControllerClose(pad);
    }
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    SettingsWindow *win = (SettingsWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (message) {
        case WM_COMMAND: {
            if (!win) break;
            int id = LOWORD(wparam);
            if (id == ID_OK) {
                store_settings(win);
                win->ok = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == ID_CANCEL || id == IDCANCEL) {
                win->ok = false;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == ID_CLEAR) {
                win->rom[0] = 0;
                SetWindowTextW(win->rom_label, L"(none)");
                SetWindowTextW(win->status, L"The ROM path was cleared; save to keep it.");
                return 0;
            }
            if (id == ID_BROWSE) {
                wchar_t path[NGR_MAX_PATH];
                _snwprintf_s(path, NGR_MAX_PATH, _TRUNCATE, L"%ls", win->rom);
                if (ngr_pick_rom(hwnd, path, NGR_MAX_PATH, L"Select the Ninja Gaiden ROM")) {
                    _snwprintf_s(win->rom, NGR_MAX_PATH, _TRUNCATE, L"%ls", path);
                    SetWindowTextW(win->rom_label, path);
                    SetWindowTextW(win->status, L"");
                }
                return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED) {
                for (int i = 0; i < 8; ++i) {
                    if (win->key_button[i] && (HWND)lparam == win->key_button[i]) {
                        win->capture_key = i;
                        win->capture_pad = CAPTURE_NONE;
                        SetWindowTextW(win->key_button[i], L"press a key");
                        SetFocus(hwnd);
                        SetWindowTextW(win->status, L"Press the key to use for this button, or Escape to cancel.");
                        return 0;
                    }
                    if (win->pad_button[i] && (HWND)lparam == win->pad_button[i]) {
                        win->capture_pad = i;
                        win->capture_key = CAPTURE_NONE;
                        SetWindowTextW(win->pad_button[i], L"press a button");
                        SetFocus(hwnd);
                        SetWindowTextW(win->status, L"Press the controller button to use, or Escape to cancel.");
                        return 0;
                    }
                }
            }
            break;
        }
        case WM_KEYDOWN: {
            if (!win) break;
            if (win->capture_key >= 0) {
                int scancode = vk_to_scancode(wparam, lparam);
                if (wparam == VK_ESCAPE) {
                    win->capture_key = CAPTURE_NONE;
                } else if (scancode != SDL_SCANCODE_UNKNOWN) {
                    const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);
                    if (name) ngr_ini_set(&win->ini, "Keyboard.Player1", NGR_BUTTON_KEYS[win->capture_key], name);
                    win->capture_key = CAPTURE_NONE;
                } else {
                    return 0;
                }
                update_key_labels(win);
                SetWindowTextW(win->status, L"");
                return 0;
            }
            if (win->capture_pad >= 0) {
                if (wparam == VK_ESCAPE) {
                    win->capture_pad = CAPTURE_NONE;
                    update_key_labels(win);
                    SetWindowTextW(win->status, L"");
                }
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                win->ok = false;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_TIMER: {
            if (!win) break;
            poll_gamepad(win);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            if (!win) break;
            HDC dc = (HDC)wparam;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0x20, 0x20, 0x20));
            static HBRUSH brush;
            if (!brush) brush = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            return (LRESULT)brush;
        }
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wparam;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0x20, 0x20, 0x20));
            static HBRUSH brush;
            if (!brush) brush = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            return (LRESULT)brush;
        }
        case WM_DESTROY: {
            if (win) {
                if (win->timer) KillTimer(hwnd, win->timer);
                if (win->sdl_ready) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            PostQuitMessage(0);
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

/* Shows the settings window; returns true when the player saved. */
bool ngr_settings_show(HWND owner, const wchar_t *config_path, const wchar_t *rom_path,
                        wchar_t *rom_out, size_t rom_out_len)
{
    SettingsWindow win;
    memset(&win, 0, sizeof(win));
    win.capture_key = CAPTURE_NONE;
    win.capture_pad = CAPTURE_NONE;
    ngr_ini_init(&win.ini);
    ngr_ini_load(&win.ini, config_path);
    if (rom_path && *rom_path) _snwprintf_s(win.rom, NGR_MAX_PATH, _TRUNCATE, L"%ls", rom_path);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = settings_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(INT_PTR)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"NinjaGaidenSettings";
    RegisterClassExW(&wc);

    HFONT font_body = ngr_font(9, FW_NORMAL, false);
    HFONT font_bold = ngr_font(15, FW_BOLD, false);
    HFONT font_small = ngr_font(8, FW_NORMAL, false);

    win.hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"NinjaGaidenSettings",
                               L"Ninja Gaiden - Settings",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, DLG_WIDTH, DLG_HEIGHT,
                               owner, NULL, GetModuleHandleW(NULL), &win);
    if (!win.hwnd) {
        DeleteObject(font_body);
        DeleteObject(font_bold);
        DeleteObject(font_small);
        ngr_ini_free(&win.ini);
        return false;
    }
    SetWindowLongPtrW(win.hwnd, GWLP_USERDATA, (LONG_PTR)&win);
    create_controls(&win, font_body, font_bold, font_small);
    load_settings(&win);

    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) win.sdl_ready = true;
    } else {
        win.sdl_ready = true;
    }
    win.timer = (int)SetTimer(win.hwnd, 1, 60, NULL);

    ShowWindow(win.hwnd, SW_SHOW);
    UpdateWindow(win.hwnd);

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if (!IsWindow(win.hwnd)) break;
        if (!IsDialogMessageW(win.hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    bool ok = win.ok;
    if (ok && rom_out) _snwprintf_s(rom_out, rom_out_len, _TRUNCATE, L"%ls", win.rom);
    DeleteObject(font_body);
    DeleteObject(font_bold);
    DeleteObject(font_small);
    ngr_ini_free(&win.ini);
    return ok;
}
