#ifndef SETH_JSON_H
#define SETH_JSON_H
#include <stddef.h>
typedef struct J {
    int type;
    char *key, *s;
    double n;
    struct J **v;
    size_t len;
} J;
enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ };
J *jnull(void), *jb(int), *jnum(double), *js(const char *), *ja(void), *jo(void);
J *jp(const char *, char **), *jc(const J *), *jg(const J *, const char *), *ji(const J *, size_t);
const char *jstr(const J *), *gs(const J *, const char *);
double gn(const J *, const char *, double);
int gb(const J *, const char *, int), jeq(const J *, const J *);
void jf(J *), jadd(J *, J *), jset(J *, const char *, J *), jdel(J *, const char *),
    jremove(J *, size_t);
char *jd(const J *, int);
#endif
