/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_prefix_temp.h"
#include <string.h>

static int starts_with_nocase(const char *text, size_t length, const char *word)
{
    size_t n = strlen(word);

    if (length < n) return 0;
    for (size_t i = 0; i < n; i++) {
        char a = text[i], b = word[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static void add(PwPrefixTemp *temp, const char *folder)
{
    for (size_t i = 0; i < temp->count; i++)
        if (!strcmp(temp->folders[i], folder)) return;
    size_t n = strlen(folder);
    if (temp->count == PW_PREFIX_TEMP_MAX || n >= PW_PREFIX_TEMP_PATH) return;
    memcpy(temp->folders[temp->count++], folder, n + 1);
}

void pw_prefix_temp_init(PwPrefixTemp *temp)
{
    memset(temp, 0, sizeof(*temp));
    add(temp, "windows/temp");
}

void pw_prefix_temp_line(PwPrefixTemp *temp, const char *line, size_t length)
{
    char folder[PW_PREFIX_TEMP_PATH];
    size_t at, out = 0, component = 0;

    if (length && line[0] == '[') {
        temp->in_environment = starts_with_nocase(line, length, "[environment]");
        return;
    }
    if (!temp->in_environment) return;
    if (starts_with_nocase(line, length, "\"temp\"=\"")) at = 8;
    else if (starts_with_nocase(line, length, "\"tmp\"=\"")) at = 7;
    else return;
    /* "C:\\users\\name\\AppData\\Local\\Temp": registry text doubles each backslash */
    if (at + 4 > length || !starts_with_nocase(line + at, length - at, "c:\\\\")) return;
    at += 4;
    for (; at < length && line[at] != '"'; at++) {
        char c = line[at];
        if (c == '\\') {
            if (at + 1 >= length || line[at + 1] != '\\') return;
            at++;
            c = '/';
        }
        if (c == ':' || c == '%' || (unsigned char)c < 0x20) return;
        if (c == '/') {
            /* an empty, "." or ".." component would leave drive_c */
            size_t start = out - component;
            if (!component || (component == 1 && folder[start] == '.') ||
                (component == 2 && folder[start] == '.' && folder[start + 1] == '.')) return;
            component = 0;
        } else {
            component++;
        }
        if (out + 1 >= sizeof(folder)) return;
        folder[out++] = c;
    }
    if (at >= length || !out) return;   /* no closing quote, or C:\ alone */
    if (folder[out - 1] == '/') out--;
    else {
        size_t start = out - component;
        if ((component == 1 && folder[start] == '.') ||
            (component == 2 && folder[start] == '.' && folder[start + 1] == '.')) return;
    }
    folder[out] = 0;
    add(temp, folder);
}
