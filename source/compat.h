/* The compatibility report as a GitHub issue link. Pure C. */
#ifndef COMPAT_H
#define COMPAT_H
typedef struct { const char *id, *value; } form_field;
int url_encode(const char *src, char *dst, int n);               /* RFC 3986, returns the length */
/* APP_REPO/issues/new?title=<title>&<id>=<value>...: a blank issue, its
 * fields prefilled; "body" is the id GitHub (web and the mobile app) fills.
 * -1 when out does not fit. */
int issue_form_url(const char *title, const form_field *f, int nf, char *out, int n);
/* The most a QR code at version 25 with ECC low holds in byte mode: the
 * link of Help's code stays within it. */
#define COMPAT_QR_BYTES 1273
/* The link of title and body plus the first of the nalt lines in alt (the
 * most detailed first) that keeps it within limit bytes, or with none of
 * them when none does: the line goes into body, which has room for size
 * bytes. The link's length, -1 when the body alone does not fit. */
int compat_fit_url(const char *title, char *body, int size, const char *const *alt, int nalt, char *out, int n, int limit);
#endif
