/*
 * build_runner.c - build the native recompilation from the player's ROM.
 *
 * The pipeline is the one the project uses, run from the launcher:
 *   1. the host recompiler turns the ROM into C (one translation unit per
 *      4 KiB PRG bank, plus a dispatch table),
 *   2. CMake configures the game project against the local engine,
 *   3. CMake compiles the native executable,
 *   4. the game plays a recorded route headless and writes the addresses the
 *      interpreter reached, so the next build compiles them natively too,
 *   5. the project is configured and compiled again with those addresses.
 *
 * The work runs on a thread so the window stays responsive; the UI reads the
 * collected output through ngr_build_output().
 */
#include "launcher.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NGR_BUILD_THREAD_SLEEP 60

struct NgrBuild {
    HANDLE          thread;
    CRITICAL_SECTION lock;
    char           *output;
    size_t          output_len;
    size_t          output_cap;
    char            status[256];
    char            error[512];
    bool            finished;
    bool            succeeded;
    volatile bool   cancel;

    HWND  hwnd;
    UINT  msg;

    wchar_t rom[NGR_MAX_PATH];
    wchar_t package_root[NGR_MAX_PATH];
    wchar_t recompiler[NGR_MAX_PATH];
    wchar_t route[NGR_MAX_PATH];
    wchar_t seed[NGR_MAX_PATH];
    wchar_t build_dir[NGR_MAX_PATH];
    wchar_t codegen_dir[NGR_MAX_PATH];
    wchar_t game_exe[NGR_MAX_PATH];
};

/* ------------------------------------------------------------- plumbing --- */

static void build_append(NgrBuild *build, const char *text)
{
    size_t len = strlen(text);
    EnterCriticalSection(&build->lock);
    if (build->output_len + len + 1 > build->output_cap) {
        size_t capacity = build->output_cap ? build->output_cap : 4096;
        while (capacity < build->output_len + len + 1) capacity *= 2;
        char *grown = (char *)realloc(build->output, capacity);
        if (grown) {
            build->output = grown;
            build->output_cap = capacity;
        }
    }
    if (build->output) {
        memcpy(build->output + build->output_len, text, len);
        build->output_len += len;
        build->output[build->output_len] = 0;
    }
    LeaveCriticalSection(&build->lock);
}

static void build_logf(NgrBuild *build, const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 3) n = (int)sizeof(line) - 3;
    line[n] = '\r';
    line[n + 1] = '\n';
    line[n + 2] = 0;
    build_append(build, line);
}

static void build_status(NgrBuild *build, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    EnterCriticalSection(&build->lock);
    vsnprintf(build->status, sizeof(build->status), format, args);
    LeaveCriticalSection(&build->lock);
    va_end(args);
}

static void build_error(NgrBuild *build, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    EnterCriticalSection(&build->lock);
    vsnprintf(build->error, sizeof(build->error), format, args);
    LeaveCriticalSection(&build->lock);
    va_end(args);
}

/* --------------------------------------------------------------- tools --- */

static bool find_in_path(const wchar_t *tool, wchar_t *out, size_t out_len)
{
    wchar_t path[4096];
    DWORD n = GetEnvironmentVariableW(L"PATH", path, 4096);
    if (!n || n >= 4096) return false;
    const wchar_t *cursor = path;
    while (*cursor) {
        const wchar_t *sep = wcschr(cursor, L';');
        size_t len = sep ? (size_t)(sep - cursor) : wcslen(cursor);
        if (len && len < MAX_PATH) {
            wchar_t candidate[MAX_PATH];
            _snwprintf_s(candidate, MAX_PATH, _TRUNCATE, L"%.*ls\\%ls", (int)len, cursor, tool);
            if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES) {
                _snwprintf_s(out, out_len, _TRUNCATE, L"%ls", candidate);
                return true;
            }
        }
        if (!sep) break;
        cursor = sep + 1;
    }
    return false;
}

static bool find_cmake(wchar_t *out, size_t out_len)
{
    if (find_in_path(L"cmake.exe", out, out_len)) return true;
    static const wchar_t *candidates[] = {
        L"C:\\Program Files\\CMake\\bin\\cmake.exe",
        L"C:\\Program Files (x86)\\CMake\\bin\\cmake.exe",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (ngr_file_exists(candidates[i])) {
            _snwprintf_s(out, out_len, _TRUNCATE, L"%ls", candidates[i]);
            return true;
        }
    }
    return false;
}

/* The CMake generator to use: a Visual Studio installation when there is one
 * (the compiler is then found by CMake itself), otherwise Ninja with a
 * compiler on PATH. */
static bool detect_generator(wchar_t *out, size_t out_len)
{
    wchar_t vswhere[MAX_PATH] = L"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (ngr_file_exists(vswhere)) {
        wchar_t command[2048];
        _snwprintf_s(command, 2048, _TRUNCATE,
                     L"\"%ls\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
                     L"-property installationVersion", vswhere);
        FILE *pipe = _wpopen(command, L"r");
        if (pipe) {
            char version[64] = { 0 };
            if (fgets(version, (int)sizeof(version), pipe)) {
                int major = atoi(version);
                const char *name = NULL;
                switch (major) {
                    case 18: name = "Visual Studio 18 2026"; break;
                    case 17: name = "Visual Studio 17 2022"; break;
                    case 16: name = "Visual Studio 16 2019"; break;
                    case 15: name = "Visual Studio 15 2017"; break;
                    default: break;
                }
                if (name) {
                    _snwprintf_s(out, out_len, _TRUNCATE, L"%hs", name);
                    _pclose(pipe);
                    return true;
                }
            }
            _pclose(pipe);
        }
    }
    wchar_t ninja[MAX_PATH];
    if (find_in_path(L"ninja.exe", ninja, sizeof(ninja))) {
        bool compiler = false;
        static const wchar_t *compilers[] = { L"cl.exe", L"gcc.exe", L"clang.exe" };
        for (size_t i = 0; i < sizeof(compilers) / sizeof(compilers[0]); ++i) {
            wchar_t found[MAX_PATH];
            if (find_in_path(compilers[i], found, sizeof(found))) { compiler = true; break; }
        }
        if (compiler) {
            _snwprintf_s(out, out_len, _TRUNCATE, L"Ninja");
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------- process runner --- */

static void append_quoted(wchar_t *cmd, size_t cmd_len, const wchar_t *arg)
{
    size_t used = wcslen(cmd);
    bool needs_quotes = wcschr(arg, L' ') || wcschr(arg, L'\t');
    if (needs_quotes) {
        if (used + 2 < cmd_len) cmd[used++] = L'"';
    }
    for (const wchar_t *c = arg; *c; ++c) {
        if (used + 3 >= cmd_len) break;
        if (*c == L'"') { cmd[used++] = L'\\'; cmd[used++] = L'"'; }
        else cmd[used++] = *c;
    }
    if (needs_quotes && used + 2 < cmd_len) cmd[used++] = L'"';
    if (used + 1 < cmd_len) cmd[used] = 0;
}

/* Runs a program, streams its output into the build log and waits for it. */
static bool run_tool(NgrBuild *build, const wchar_t *exe, const wchar_t *const *args,
                     const wchar_t *cwd, int *exit_code)
{
    wchar_t command[8192];
    _snwprintf_s(command, 8192, _TRUNCATE, L"\"%ls\"", exe);
    for (size_t i = 0; args && args[i]; ++i) {
        size_t used = wcslen(command);
        if (used + 1 < 8192) command[used++] = L' ';
        command[used] = 0;
        append_quoted(command, 8192, args[i]);
    }

    SECURITY_ATTRIBUTES security;
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = NULL;
    security.bInheritHandle = TRUE;
    HANDLE read_pipe = NULL, write_pipe = NULL;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) return false;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));
    BOOL created = CreateProcessW(exe, command, NULL, NULL, TRUE,
                                   CREATE_NO_WINDOW, NULL, cwd, &startup, &process);
    CloseHandle(write_pipe);
    if (!created) {
        CloseHandle(read_pipe);
        build_error(build, "Could not start %ls (error %lu).", exe, GetLastError());
        return false;
    }

    char buffer[4096];
    char line[8192];
    size_t line_len = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(read_pipe, buffer, (DWORD)sizeof(buffer), &got, NULL) || got == 0) break;
        for (DWORD i = 0; i < got; ++i) {
            char c = buffer[i];
            if (c == '\n') {
                line[line_len] = 0;
                build_append(build, line);
                build_append(build, "\r\n");
                line_len = 0;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = c;
            }
        }
    }
    if (line_len) {
        line[line_len] = 0;
        build_append(build, line);
        build_append(build, "\r\n");
    }
    CloseHandle(read_pipe);

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    if (exit_code) *exit_code = (int)code;
    return true;
}

/* ------------------------------------------------------------- the steps --- */

static bool seed_file_has_data(const wchar_t *path)
{
    FILE *file = NULL;
    if (_wfopen_s(&file, path, L"r") != 0 || !file) return false;
    char line[256];
    bool found = false;
    while (fgets(line, (int)sizeof(line), file)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p && *p != '#' && *p != '\n' && *p != '\r') { found = true; break; }
    }
    fclose(file);
    return found;
}

static void ensure_seed_file(NgrBuild *build, const wchar_t *path)
{
    if (ngr_file_exists(path)) return;
    wchar_t dir[NGR_MAX_PATH];
    _snwprintf_s(dir, NGR_MAX_PATH, _TRUNCATE, L"%ls", path);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) {
        *slash = 0;
        CreateDirectoryW(dir, NULL);
    }
    ngr_write_text(path, "# Cycle seed file: the addresses the interpreter executed.\n"
                         "# Filled in by the launcher after the first build.\n");
    build_logf(build, "Created an empty seed file at %ls", path);
}

static bool generate_sources(NgrBuild *build)
{
    wchar_t game_toml[NGR_MAX_PATH];
    ngr_join(game_toml, NGR_MAX_PATH, build->package_root, L"game.toml");
    const wchar_t *args[] = { build->rom, L"--game", game_toml, L"--cycle-accurate",
                              L"--output-prefix", L"game", NULL };
    build_status(build, "Generating the native C source");
    build_logf(build, "> %ls %ls --game %ls --cycle-accurate --output-prefix game",
               build->recompiler, build->rom, game_toml);
    int code = 0;
    if (!run_tool(build, build->recompiler, args, build->codegen_dir, &code)) return false;
    if (code != 0) {
        build_error(build, "The recompiler failed with exit code %d.", code);
        return false;
    }
    return true;
}

static bool configure_project(NgrBuild *build, const wchar_t *cmake, const wchar_t *generator,
                              const wchar_t *engine_root)
{
    wchar_t build_dir[NGR_MAX_PATH];
    ngr_join(build_dir, NGR_MAX_PATH, build->package_root, L"out");
    _snwprintf_s(build->build_dir, NGR_MAX_PATH, _TRUNCATE, L"%ls", build_dir);

    wchar_t rom_arg[NGR_MAX_PATH];
    _snwprintf_s(rom_arg, NGR_MAX_PATH, _TRUNCATE, L"-DNESRECOMP_ROM=%ls", build->rom);
    wchar_t recompiler_arg[NGR_MAX_PATH];
    _snwprintf_s(recompiler_arg, NGR_MAX_PATH, _TRUNCATE, L"-DNESRECOMP_RECOMPILER=%ls", build->recompiler);
    wchar_t root_arg[NGR_MAX_PATH];
    _snwprintf_s(root_arg, NGR_MAX_PATH, _TRUNCATE, L"-DNESRECOMP_ROOT=%ls", engine_root);

    const wchar_t *args[] = { L"-S", build->package_root, L"-B", build->build_dir,
                              L"-G", generator, L"-A", L"x64",
                              rom_arg, recompiler_arg, root_arg, NULL };
    build_status(build, "Configuring the build");
    build_logf(build, "> %ls -S %ls -B %ls -G \"%ls\" -A x64", cmake, build->package_root,
               build->build_dir, generator);
    int code = 0;
    if (!run_tool(build, cmake, args, build->package_root, &code)) return false;
    if (code != 0) {
        build_error(build, "CMake could not configure the project (exit code %d).", code);
        return false;
    }
    return true;
}

static bool compile_project(NgrBuild *build, const wchar_t *cmake)
{
    const wchar_t *args[] = { L"--build", build->build_dir, L"--config", L"Release",
                              L"--target", L"NinjaGaidenRecomp", NULL };
    build_status(build, "Compiling the native executable");
    build_logf(build, "> %ls --build %ls --config Release --target NinjaGaidenRecomp", cmake, build->build_dir);
    int code = 0;
    if (!run_tool(build, cmake, args, build->package_root, &code)) return false;
    if (code != 0) {
        build_error(build, "The build failed (exit code %d).", code);
        return false;
    }
    return true;
}

static bool collect_seeds(NgrBuild *build)
{
    build_status(build, "Recording the routes the game takes");
    wchar_t rom_arg[NGR_MAX_PATH];
    _snwprintf_s(rom_arg, NGR_MAX_PATH, _TRUNCATE, L"%ls", build->rom);
    /* 3200 frames of the recorded route: the title screen, the story pages
     * and the first stage, so that the game itself runs natively as well. */
    const wchar_t *args[] = { build->rom, L"--frames", L"3200", L"--headless",
                              L"--input", build->route, L"--miss-log", build->seed, NULL };
    build_logf(build, "> NinjaGaidenRecomp.exe %ls --frames 3200 --headless --input %ls --miss-log %ls",
               build->rom, build->route, build->seed);
    int code = 0;
    if (!run_tool(build, build->game_exe, args, build->build_dir, &code)) return false;
    if (code != 0) {
        build_error(build, "The coverage run failed (exit code %d).", code);
        return false;
    }
    return true;
}

static DWORD WINAPI build_thread(LPVOID parameter)
{
    NgrBuild *build = (NgrBuild *)parameter;

    wchar_t cmake[MAX_PATH];
    wchar_t generator[128];
    wchar_t engine_root[NGR_MAX_PATH];

    build_logf(build, "Ninja Gaiden native recompilation - build log");
    build_logf(build, "--------------------------------------------------");

    if (!find_cmake(cmake, sizeof(cmake))) {
        build_error(build, "CMake was not found. Install CMake 3.20 or newer "
                           "(https://cmake.org/download/) and try again.");
        goto done;
    }
    build_logf(build, "cmake: %ls", cmake);

    if (!detect_generator(generator, sizeof(generator))) {
        build_error(build, "No C++ toolchain was found. Install Visual Studio 2022 or newer with the "
                           "\"Desktop development with C++\" workload, or put Ninja and a C compiler on PATH.");
        goto done;
    }
    build_logf(build, "generator: %ls", generator);

    wchar_t packaged_engine[NGR_MAX_PATH];
    ngr_join(packaged_engine, NGR_MAX_PATH, build->package_root, L"engine");
    if (ngr_dir_exists(packaged_engine)) {
        _snwprintf_s(engine_root, NGR_MAX_PATH, _TRUNCATE, L"%ls", packaged_engine);
    } else {
#ifdef NGR_ENGINE_ROOT
        _snwprintf_s(engine_root, NGR_MAX_PATH, _TRUNCATE, L"%hs", NGR_ENGINE_ROOT);
#else
        build_error(build, "The NESRecomp engine was not found. Expected an \"engine\" folder next to the "
                           "launcher, or a configured NESRECOMP_ROOT.");
        goto done;
#endif
    }
    if (!ngr_dir_exists(engine_root)) {
        build_error(build, "The NESRecomp engine folder \"%ls\" does not exist.", engine_root);
        goto done;
    }
    build_logf(build, "engine: %ls", engine_root);

    if (!ngr_file_exists(build->recompiler)) {
        build_error(build, "The host recompiler \"%ls\" is missing from the package.", build->recompiler);
        goto done;
    }
    ensure_seed_file(build, build->seed);

    if (!generate_sources(build)) goto done;
    if (build->cancel) goto done;
    if (!configure_project(build, cmake, generator, engine_root)) goto done;
    if (build->cancel) goto done;
    if (!compile_project(build, cmake)) goto done;
    if (build->cancel) goto done;

    wchar_t release_dir[NGR_MAX_PATH];
    ngr_join(release_dir, NGR_MAX_PATH, build->build_dir, L"Release");
    ngr_join(build->game_exe, NGR_MAX_PATH, release_dir, L"NinjaGaidenRecomp.exe");
    if (!ngr_file_exists(build->game_exe)) {
        wchar_t flat[NGR_MAX_PATH];
        ngr_join(flat, NGR_MAX_PATH, build->build_dir, L"NinjaGaidenRecomp.exe");
        if (ngr_file_exists(flat)) _snwprintf_s(build->game_exe, NGR_MAX_PATH, _TRUNCATE, L"%ls", flat);
    }
    if (!ngr_file_exists(build->game_exe)) {
        build_error(build, "The build finished but NinjaGaidenRecomp.exe was not produced.");
        goto done;
    }

    /* The first build compiles only what discovery reached.  Playing a
     * recorded route shows the recompiler which addresses the program runs,
     * and the second build compiles those as native code as well. */
    if (!seed_file_has_data(build->seed)) {
        if (!ngr_file_exists(build->route)) {
            build_logf(build, "No route file: the build keeps the addresses discovery found.");
        } else if (!collect_seeds(build)) {
            goto done;
        } else {
            build_logf(build, "Recorded the routes; rebuilding with the addresses it reached.");
            if (!configure_project(build, cmake, generator, engine_root)) goto done;
            if (!compile_project(build, cmake)) goto done;
        }
    }

    build_status(build, "The game is ready");
    build->succeeded = true;
    build_logf(build, "--------------------------------------------------");
    build_logf(build, "Done. NinjaGaidenRecomp.exe is ready to play.");

done:
    build->finished = true;
    if (!build->succeeded && !build->error[0]) {
        if (build->cancel) build_error(build, "The build was cancelled.");
        else build_error(build, "The build did not finish.");
    }
    if (build->hwnd) PostMessageW(build->hwnd, build->msg, 0, 0);
    return 0;
}

NgrBuild *ngr_build_start(const wchar_t *rom_path, const wchar_t *package_root,
                           const wchar_t *recompiler, const wchar_t *route_path,
                           const wchar_t *seed_path, HWND hwnd, UINT msg)
{
    NgrBuild *build = (NgrBuild *)calloc(1, sizeof(*build));
    if (!build) return NULL;
    InitializeCriticalSection(&build->lock);
    _snwprintf_s(build->rom, NGR_MAX_PATH, _TRUNCATE, L"%ls", rom_path);
    _snwprintf_s(build->package_root, NGR_MAX_PATH, _TRUNCATE, L"%ls", package_root);
    _snwprintf_s(build->recompiler, NGR_MAX_PATH, _TRUNCATE, L"%ls", recompiler);
    _snwprintf_s(build->route, NGR_MAX_PATH, _TRUNCATE, L"%ls", route_path);
    _snwprintf_s(build->seed, NGR_MAX_PATH, _TRUNCATE, L"%ls", seed_path);
    build->hwnd = hwnd;
    build->msg = msg;
    ngr_join(build->codegen_dir, NGR_MAX_PATH, package_root, L"out\\codegen");
    ngr_make_dirs(build->codegen_dir);

    build->thread = CreateThread(NULL, 0, build_thread, build, 0, NULL);
    if (!build->thread) {
        DeleteCriticalSection(&build->lock);
        free(build);
        return NULL;
    }
    return build;
}

void ngr_build_stop(NgrBuild *build)
{
    if (!build) return;
    build->cancel = true;
}

void ngr_build_free(NgrBuild *build)
{
    if (!build) return;
    if (build->thread) {
        WaitForSingleObject(build->thread, 5000);
        CloseHandle(build->thread);
    }
    DeleteCriticalSection(&build->lock);
    free(build->output);
    free(build);
}

bool ngr_build_finished(NgrBuild *build)
{
    return build && build->finished;
}

bool ngr_build_succeeded(NgrBuild *build)
{
    return build && build->succeeded;
}

const char *ngr_build_status(NgrBuild *build)
{
    if (!build) return "";
    EnterCriticalSection(&build->lock);
    static char copy[256];
    snprintf(copy, sizeof(copy), "%s", build->status);
    LeaveCriticalSection(&build->lock);
    return copy;
}

const char *ngr_build_error(NgrBuild *build)
{
    if (!build) return "";
    EnterCriticalSection(&build->lock);
    static char copy[512];
    snprintf(copy, sizeof(copy), "%s", build->error);
    LeaveCriticalSection(&build->lock);
    return copy;
}

const char *ngr_build_output(NgrBuild *build)
{
    if (!build) return "";
    EnterCriticalSection(&build->lock);
    static char *copy = NULL;
    size_t len = build->output_len;
    char *grown = (char *)realloc(copy, len + 1);
    if (grown) {
        copy = grown;
        memcpy(copy, build->output, len);
        copy[len] = 0;
    }
    LeaveCriticalSection(&build->lock);
    return copy ? copy : "";
}

size_t ngr_build_output_length(NgrBuild *build)
{
    if (!build) return 0;
    EnterCriticalSection(&build->lock);
    size_t len = build->output_len;
    LeaveCriticalSection(&build->lock);
    return len;
}
