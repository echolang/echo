/*
 * Windows half of the epm shim: directory, stat, mkdir, a process runner.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static wchar_t *utf8_to_wide(const char *utf8)
{
    int needed;
    wchar_t *wide;

    if (utf8 == NULL) {
        return NULL;
    }

    needed = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (needed <= 0) {
        return NULL;
    }

    wide = malloc((size_t)needed * sizeof(wchar_t));
    if (wide == NULL) {
        return NULL;
    }

    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, needed) <= 0) {
        free(wide);
        return NULL;
    }

    return wide;
}

static char *wide_to_utf8(const wchar_t *wide)
{
    int needed;
    char *utf8;

    if (wide == NULL) {
        return NULL;
    }

    needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) {
        return NULL;
    }

    utf8 = malloc((size_t)needed);
    if (utf8 == NULL) {
        return NULL;
    }

    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, needed, NULL, NULL) <= 0) {
        free(utf8);
        return NULL;
    }

    return utf8;
}

static void slash_to_backslash(wchar_t *path)
{
    wchar_t *p;

    for (p = path; *p != L'\0'; p++) {
        if (*p == L'/') {
            *p = L'\\';
        }
    }
}

static int is_unc(const wchar_t *path)
{
    return path[0] == L'\\' && path[1] == L'\\';
}

/* absolute Win32 path with a \\?\ prefix so vendor trees past MAX_PATH work */
static wchar_t *wide_ntpath(const char *utf8)
{
    wchar_t *wide;
    wchar_t *full;
    DWORD needed;
    DWORD n;
    wchar_t *out;

    wide = utf8_to_wide(utf8);
    if (wide == NULL) {
        return NULL;
    }

    slash_to_backslash(wide);

    if (wcsncmp(wide, L"\\\\?\\", 4) == 0) {
        return wide;
    }

    needed = GetFullPathNameW(wide, 0, NULL, NULL);
    if (needed == 0) {
        free(wide);
        return NULL;
    }

    full = malloc((size_t)needed * sizeof(wchar_t));
    if (full == NULL) {
        free(wide);
        return NULL;
    }

    n = GetFullPathNameW(wide, needed, full, NULL);
    free(wide);
    if (n == 0 || n >= needed) {
        free(full);
        return NULL;
    }

    if (is_unc(full)) {
        /* \\server\share -> \\?\UNC\server\share */
        size_t n = wcslen(full);
        out = malloc((8 + n + 1) * sizeof(wchar_t));
        if (out == NULL) {
            free(full);
            return NULL;
        }
        memcpy(out, L"\\\\?\\UNC", 7 * sizeof(wchar_t));
        memcpy(out + 7, full + 1, n * sizeof(wchar_t));
        free(full);
        return out;
    }

    {
        size_t n = wcslen(full);
        out = malloc((4 + n + 1) * sizeof(wchar_t));
        if (out == NULL) {
            free(full);
            return NULL;
        }
        memcpy(out, L"\\\\?\\", 4 * sizeof(wchar_t));
        memcpy(out + 4, full, (n + 1) * sizeof(wchar_t));
        free(full);
        return out;
    }
}

typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAW data;
    int first;
    int done;
} epm_dir;

void *epm_opendir(const char *path)
{
    epm_dir *dir;
    wchar_t *wide;
    wchar_t *pattern;
    size_t n;

    if (path == NULL || path[0] == '\0') {
        return NULL;
    }

    wide = wide_ntpath(path);
    if (wide == NULL) {
        return NULL;
    }

    n = wcslen(wide);
    pattern = malloc((n + 3) * sizeof(wchar_t));
    if (pattern == NULL) {
        free(wide);
        return NULL;
    }

    memcpy(pattern, wide, (n + 1) * sizeof(wchar_t));
    if (pattern[n - 1] != L'\\') {
        pattern[n] = L'\\';
        n += 1;
    }
    pattern[n] = L'*';
    pattern[n + 1] = L'\0';
    free(wide);

    dir = calloc(1, sizeof(*dir));
    if (dir == NULL) {
        free(pattern);
        return NULL;
    }

    dir->handle = FindFirstFileW(pattern, &dir->data);
    free(pattern);
    if (dir->handle == INVALID_HANDLE_VALUE) {
        free(dir);
        return NULL;
    }

    dir->first = 1;
    return dir;
}

int32_t epm_readdir(void *handle, char *name, uint64_t cap)
{
    epm_dir *dir = handle;
    char *utf8;
    size_t n;

    if (dir == NULL || name == NULL || cap == 0) {
        return -1;
    }

    for (;;) {
        if (dir->done) {
            return 0;
        }

        if (!dir->first) {
            if (!FindNextFileW(dir->handle, &dir->data)) {
                dir->done = 1;
                return 0;
            }
        }
        dir->first = 0;

        if (wcscmp(dir->data.cFileName, L".") == 0
            || wcscmp(dir->data.cFileName, L"..") == 0) {
            continue;
        }

        utf8 = wide_to_utf8(dir->data.cFileName);
        if (utf8 == NULL) {
            return -1;
        }

        n = strlen(utf8);
        if (n + 1 > cap) {
            free(utf8);
            return -1;
        }

        memcpy(name, utf8, n + 1);
        free(utf8);
        return 1;
    }
}

void epm_closedir(void *handle)
{
    epm_dir *dir = handle;

    if (dir == NULL) {
        return;
    }

    if (dir->handle != INVALID_HANDLE_VALUE && dir->handle != NULL) {
        FindClose(dir->handle);
    }

    free(dir);
}

int32_t epm_kind(const char *path)
{
    wchar_t *wide;
    DWORD attributes;

    if (path == NULL) {
        return -1;
    }

    wide = wide_ntpath(path);
    if (wide == NULL) {
        return -1;
    }

    attributes = GetFileAttributesW(wide);
    free(wide);

    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return 0;
        }
        return -1;
    }

    return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
}

int32_t epm_mkdir(const char *path)
{
    wchar_t *wide;
    BOOL ok;

    if (path == NULL) {
        return -1;
    }

    wide = wide_ntpath(path);
    if (wide == NULL) {
        return -1;
    }

    ok = CreateDirectoryW(wide, NULL);
    free(wide);

    if (ok) {
        return 0;
    }

    return GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
}

int32_t epm_rmdir(const char *path)
{
    wchar_t *wide;
    BOOL ok;

    if (path == NULL) {
        return -1;
    }

    wide = wide_ntpath(path);
    if (wide == NULL) {
        return -1;
    }

    ok = RemoveDirectoryW(wide);
    free(wide);
    return ok ? 0 : -1;
}

int32_t epm_rename(const char *from, const char *to)
{
    wchar_t *wfrom;
    wchar_t *wto;
    BOOL ok;

    if (from == NULL || to == NULL) {
        return -1;
    }

    wfrom = wide_ntpath(from);
    wto = wide_ntpath(to);
    if (wfrom == NULL || wto == NULL) {
        free(wfrom);
        free(wto);
        return -1;
    }

    ok = MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING);
    free(wfrom);
    free(wto);
    return ok ? 0 : -1;
}

static int quote_windows_arg(const char *arg, char **out)
{
    size_t i;
    size_t n;
    int need_quote = 0;
    char *dst;
    size_t o = 0;

    if (arg == NULL) {
        return -1;
    }

    n = strlen(arg);
    if (n == 0) {
        need_quote = 1;
    }

    for (i = 0; i < n; i++) {
        if (arg[i] == ' ' || arg[i] == '\t' || arg[i] == '"') {
            need_quote = 1;
        }
    }

    dst = malloc(n * 2 + 4);
    if (dst == NULL) {
        return -1;
    }

    if (!need_quote) {
        memcpy(dst, arg, n + 1);
        *out = dst;
        return 0;
    }

    dst[o++] = '"';
    for (i = 0; i < n; i++) {
        size_t slashes = 0;
        while (i < n && arg[i] == '\\') {
            slashes += 1;
            i += 1;
        }
        if (i == n) {
            while (slashes--) {
                dst[o++] = '\\';
                dst[o++] = '\\';
            }
            break;
        }
        if (arg[i] == '"') {
            while (slashes--) {
                dst[o++] = '\\';
                dst[o++] = '\\';
            }
            dst[o++] = '\\';
            dst[o++] = '"';
        }
        else {
            while (slashes--) {
                dst[o++] = '\\';
            }
            dst[o++] = arg[i];
        }
    }
    dst[o++] = '"';
    dst[o] = '\0';
    *out = dst;
    return 0;
}

static int is_cmd_exe(const char *name)
{
    const char *slash;
    const char *base;

    if (name == NULL) {
        return 0;
    }

    slash = strrchr(name, '\\');
    if (slash == NULL) {
        slash = strrchr(name, '/');
    }
    base = slash == NULL ? name : slash + 1;

    return _stricmp(base, "cmd") == 0 || _stricmp(base, "cmd.exe") == 0;
}

static wchar_t *windows_command_line(const epm_argv *a)
{
    char *line = NULL;
    size_t cap = 0;
    size_t len = 0;
    int32_t i;
    wchar_t *wide;

    /* cmd's /c remainder is a command string, not an argv word. quoting it
     * the CommandLineToArgvW way is the encoding cmd then misreads. same
     * exception Compiler::quote_windows_arg's tests pin. */
    if (a->n >= 3 && strcmp(a->v[1], "/c") == 0 && is_cmd_exe(a->v[0])) {
        char *quoted = NULL;
        size_t qn;
        size_t pn;

        if (quote_windows_arg(a->v[0], &quoted) != 0) {
            return NULL;
        }

        qn = strlen(quoted);
        pn = strlen(a->v[2]);
        line = malloc(qn + pn + 6);
        if (line == NULL) {
            free(quoted);
            return NULL;
        }
        memcpy(line, quoted, qn);
        memcpy(line + qn, " /c ", 4);
        memcpy(line + qn + 4, a->v[2], pn + 1);
        free(quoted);
        wide = utf8_to_wide(line);
        free(line);
        return wide;
    }

    for (i = 0; i < a->n; i++) {
        char *quoted = NULL;
        size_t qn;

        if (quote_windows_arg(a->v[i], &quoted) != 0) {
            free(line);
            return NULL;
        }

        qn = strlen(quoted);
        if (epm_grow(&line, &cap, len + qn + 2) != 0) {
            free(quoted);
            free(line);
            return NULL;
        }

        if (len > 0) {
            line[len++] = ' ';
        }
        memcpy(line + len, quoted, qn);
        len += qn;
        line[len] = '\0';
        free(quoted);
    }

    if (line == NULL) {
        return NULL;
    }

    wide = utf8_to_wide(line);
    free(line);
    return wide;
}

static int drain_pipe(HANDLE pipe, char **buf, size_t *cap, size_t *len)
{
    char tmp[4096];
    DWORD got = 0;

    for (;;) {
        if (!ReadFile(pipe, tmp, sizeof(tmp), &got, NULL)) {
            return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
        }

        if (got == 0) {
            return 0;
        }

        if (epm_grow(buf, cap, *len + (size_t)got + 1) != 0) {
            return -1;
        }

        memcpy(*buf + *len, tmp, (size_t)got);
        *len += (size_t)got;
    }
}

static int read_available(HANDLE pipe, char **buf, size_t *cap, size_t *len)
{
    DWORD avail = 0;
    DWORD got = 0;

    if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL)) {
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    }

    if (avail == 0) {
        return 1;
    }

    if (epm_grow(buf, cap, *len + (size_t)avail + 1) != 0) {
        return -1;
    }

    if (!ReadFile(pipe, *buf + *len, avail, &got, NULL)) {
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    }

    *len += (size_t)got;
    return 1;
}

static void close_pipes(HANDLE out_r, HANDLE out_w, HANDLE err_r, HANDLE err_w)
{
    if (out_r) CloseHandle(out_r);
    if (out_w) CloseHandle(out_w);
    if (err_r) CloseHandle(err_r);
    if (err_w) CloseHandle(err_w);
}

int32_t epm_run(
    void *handle,
    char **out_s, int32_t *out_n,
    char **err_s, int32_t *err_n,
    int32_t *code
)
{
    epm_argv *a = handle;
    SECURITY_ATTRIBUTES sa;
    HANDLE out_r = NULL;
    HANDLE out_w = NULL;
    HANDLE err_r = NULL;
    HANDLE err_w = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t *cmdline;
    wchar_t *app = NULL;
    char *out_buf = NULL;
    char *err_buf = NULL;
    size_t out_cap = 256;
    size_t err_cap = 256;
    size_t out_len = 0;
    size_t err_len = 0;
    DWORD exit_code = 1;

    if (a == NULL || a->n == 0 || a->v == NULL || a->v[0] == NULL) {
        return -1;
    }

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0)) {
        close_pipes(out_r, out_w, err_r, err_w);
        return -1;
    }

    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    cmdline = windows_command_line(a);
    if (cmdline == NULL) {
        close_pipes(out_r, out_w, err_r, err_w);
        return -1;
    }

    if (strchr(a->v[0], '/') != NULL || strchr(a->v[0], '\\') != NULL
        || (a->v[0][0] != '\0' && a->v[0][1] == ':')) {
        app = wide_ntpath(a->v[0]);
    }

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_w;
    si.hStdError = err_w;

    if (!CreateProcessW(
            app, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        free(app);
        free(cmdline);
        close_pipes(out_r, out_w, err_r, err_w);
        return -1;
    }

    free(app);
    free(cmdline);
    CloseHandle(out_w);
    CloseHandle(err_w);
    CloseHandle(pi.hThread);

    out_buf = malloc(out_cap);
    err_buf = malloc(err_cap);
    if (out_buf == NULL || err_buf == NULL) {
        free(out_buf);
        free(err_buf);
        CloseHandle(out_r);
        CloseHandle(err_r);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return -1;
    }

    for (;;) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 15);
        if (read_available(out_r, &out_buf, &out_cap, &out_len) < 0
            || read_available(err_r, &err_buf, &err_cap, &err_len) < 0) {
            free(out_buf);
            free(err_buf);
            CloseHandle(out_r);
            CloseHandle(err_r);
            CloseHandle(pi.hProcess);
            return -1;
        }
        if (wait == WAIT_OBJECT_0) {
            if (drain_pipe(out_r, &out_buf, &out_cap, &out_len) < 0
                || drain_pipe(err_r, &err_buf, &err_cap, &err_len) < 0) {
                free(out_buf);
                free(err_buf);
                CloseHandle(out_r);
                CloseHandle(err_r);
                CloseHandle(pi.hProcess);
                return -1;
            }
            break;
        }
    }

    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(out_r);
    CloseHandle(err_r);

    *code = (int32_t)exit_code;
    *out_s = out_buf;
    *out_n = (int32_t)out_len;
    *err_s = err_buf;
    *err_n = (int32_t)err_len;
    return 0;
}
