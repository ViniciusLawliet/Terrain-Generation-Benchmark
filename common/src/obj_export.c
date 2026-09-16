#include "obj_export.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} StrBuf;

static void strbuf_reserve(StrBuf *s, size_t extra) {
    if (s->len + extra <= s->cap) return;
    size_t newcap = s->cap ? s->cap * 2 : (1u << 16);
    while (newcap < s->len + extra) newcap *= 2;
    s->buf = (char *)realloc(s->buf, newcap);
    s->cap = newcap;
}

static inline int uint_to_str(char *buf, unsigned long long v) {
    char tmp[24];
    int n = 0;
    if (v == 0) {
        buf[0] = '0';
        return 1;
    }
    while (v) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static inline int fixed6_to_str(char *buf, float value) {
    char *p = buf;
    if (value < 0.0f) {
        *p++ = '-';
        value = -value;
    }
    long long scaled = (long long)((double)value * 1000000.0 + 0.5);
    long long intpart = scaled / 1000000LL;
    long long frac = scaled % 1000000LL;
    p += uint_to_str(p, (unsigned long long)intpart);
    *p++ = '.';
    for (int i = 5; i >= 0; i--) {
        p[i] = (char)('0' + (frac % 10));
        frac /= 10;
    }
    p += 6;
    return (int)(p - buf);
}

static inline void append_vertex_line(StrBuf *s, float x, float y, float z) {
    strbuf_reserve(s, 80);
    char *p = s->buf + s->len;
    *p++ = 'v';
    *p++ = ' ';
    p += fixed6_to_str(p, x);
    *p++ = ' ';
    p += fixed6_to_str(p, y);
    *p++ = ' ';
    p += fixed6_to_str(p, z);
    *p++ = '\n';
    s->len = (size_t)(p - s->buf);
}

static inline void append_face_line(StrBuf *s, size_t a, size_t b, size_t c) {
    strbuf_reserve(s, 64);
    char *p = s->buf + s->len;
    *p++ = 'f';
    *p++ = ' ';
    p += uint_to_str(p, (unsigned long long)a);
    *p++ = ' ';
    p += uint_to_str(p, (unsigned long long)b);
    *p++ = ' ';
    p += uint_to_str(p, (unsigned long long)c);
    *p++ = '\n';
    s->len = (size_t)(p - s->buf);
}

int export_obj(const char *path, const TriMesh *mesh, int num_threads) {

    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    int nthreads = num_threads > 0 ? num_threads : omp_get_max_threads();

    omp_set_num_threads(nthreads);

    size_t nverts = mesh->vertex_count;
    size_t ntris = mesh->triangle_count;

    StrBuf *vchunks = (StrBuf *)calloc((size_t)nthreads, sizeof(StrBuf));
    size_t per_thread_v = nverts ? (nverts + (size_t)nthreads - 1) / (size_t)nthreads : 0;

    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        StrBuf *s = &vchunks[tid];
        size_t start = (size_t)tid * per_thread_v;
        size_t end = start + per_thread_v;
        if (end > nverts) end = nverts;
        if (start < end) {
            strbuf_reserve(s, (end - start) * 40);
            for (size_t vi = start; vi < end; vi++) {
                append_vertex_line(s, mesh->vertices[vi].x, mesh->vertices[vi].y, mesh->vertices[vi].z);
            }
        }
    }

    for (int t = 0; t < nthreads; t++) {
        if (vchunks[t].len) fwrite(vchunks[t].buf, 1, vchunks[t].len, fp);
        free(vchunks[t].buf);
    }
    free(vchunks);

    StrBuf *fchunks = (StrBuf *)calloc((size_t)nthreads, sizeof(StrBuf));
    size_t per_thread_f = ntris ? (ntris + (size_t)nthreads - 1) / (size_t)nthreads : 0;

    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        StrBuf *s = &fchunks[tid];
        size_t start = (size_t)tid * per_thread_f;
        size_t end = start + per_thread_f;
        if (end > ntris) end = ntris;
        if (start < end) {
            strbuf_reserve(s, (end - start) * 24);
            for (size_t ti = start; ti < end; ti++) {
                size_t base = ti * 3 + 1;
                append_face_line(s, base, base + 1, base + 2);
            }
        }
    }

    for (int t = 0; t < nthreads; t++) {
        if (fchunks[t].len) fwrite(fchunks[t].buf, 1, fchunks[t].len, fp);
        free(fchunks[t].buf);
    }
    free(fchunks);

    fclose(fp);
    return 0;
}