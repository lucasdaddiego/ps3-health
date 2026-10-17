#include <stdio.h>
#include <string.h>
#include "compat.h"
#include "version.h"

static int unreserved(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~';
}

int url_encode(const char *src, char *dst, int n)
{
    static const char hex[] = "0123456789ABCDEF";
    int j = 0;
    for (; *src && j + 4 < n; src++) {
        unsigned char c = (unsigned char)*src;
        if (unreserved((char)c)) dst[j++] = (char)c;
        else { dst[j++] = '%'; dst[j++] = hex[c >> 4]; dst[j++] = hex[c & 15]; }
    }
    dst[j] = 0;
    return j;
}

/* Appends "<sep><key>=<encoded value>"; -1 when it does not fit. */
static int param(char *out, int n, int j, const char *key, const char *value)
{
    int k = snprintf(out + j, n - j, "&%s=", key);
    if (k < 0 || j + k >= n) return -1;
    j += k;
    char enc[2048];
    int e = url_encode(value, enc, sizeof enc);
    if (j + e >= n) return -1;
    memcpy(out + j, enc, e + 1);
    return j + e;
}

int issue_form_url(const char *title, const form_field *f, int nf, char *out, int n)
{
    int j = snprintf(out, n, APP_REPO "/issues/new");
    if (j < 0 || j >= n) return -1;
    j = param(out, n, j, "title", title);
    if (j > 0) out[strlen(APP_REPO "/issues/new")] = '?';
    for (int i = 0; i < nf && j >= 0; i++) j = param(out, n, j, f[i].id, f[i].value);
    return j;
}

int compat_fit_url(const char *title, char *body, int size, const char *const *alt, int nalt, char *out, int n, int limit)
{
    int base = (int)strlen(body);
    const form_field f[] = {{"body", body}};
    for (int i = 0; i <= nalt; i++) {        /* each line in turn, then none */
        body[base] = 0;
        if (i < nalt) {
            if (base + (int)strlen(alt[i]) >= size) continue;
            strcpy(body + base, alt[i]);
        }
        int j = issue_form_url(title, f, 1, out, n);
        if (j >= 0 && j <= limit) return j;
    }
    body[base] = 0;
    return -1;
}
