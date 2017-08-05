#pragma once

#include <stdbool.h>
#include <math.h>

#include "ra.h"

// A 3x2 matrix, with the translation part separate.
struct gl_transform {
    // row-major, e.g. in mathematical notation:
    //  | m[0][0] m[0][1] |
    //  | m[1][0] m[1][1] |
    float m[2][2];
    float t[2];
};

static const struct gl_transform identity_trans = {
    .m = {{1.0, 0.0}, {0.0, 1.0}},
    .t = {0.0, 0.0},
};

void gl_transform_ortho(struct gl_transform *t, float x0, float x1,
                        float y0, float y1);

// This treats m as an affine transformation, in other words m[2][n] gets
// added to the output.
static inline void gl_transform_vec(struct gl_transform t, float *x, float *y)
{
    float vx = *x, vy = *y;
    *x = vx * t.m[0][0] + vy * t.m[0][1] + t.t[0];
    *y = vx * t.m[1][0] + vy * t.m[1][1] + t.t[1];
}

struct mp_rect_f {
    float x0, y0, x1, y1;
};

// Semantic equality (fuzzy comparison)
static inline bool mp_rect_f_seq(struct mp_rect_f a, struct mp_rect_f b)
{
    return fabs(a.x0 - b.x0) < 1e-6 && fabs(a.x1 - b.x1) < 1e-6 &&
           fabs(a.y0 - b.y0) < 1e-6 && fabs(a.y1 - b.y1) < 1e-6;
}

static inline void gl_transform_rect(struct gl_transform t, struct mp_rect_f *r)
{
    gl_transform_vec(t, &r->x0, &r->y0);
    gl_transform_vec(t, &r->x1, &r->y1);
}

static inline bool gl_transform_eq(struct gl_transform a, struct gl_transform b)
{
    for (int x = 0; x < 2; x++) {
        for (int y = 0; y < 2; y++) {
            if (a.m[x][y] != b.m[x][y])
                return false;
        }
    }

    return a.t[0] == b.t[0] && a.t[1] == b.t[1];
}

void gl_transform_trans(struct gl_transform t, struct gl_transform *x);

struct fbotex {
    struct ra *ra;
    struct ra_tex *tex;
    int rw, rh; // real (texture) size, same as tex->params.w/h
    int lw, lh; // logical (configured) size, <= than texture size
};

bool fbotex_init(struct fbotex *fbo, struct ra *ra, struct mp_log *log,
                 int w, int h, const struct ra_format *fmt);
void fbotex_uninit(struct fbotex *fbo);
bool fbotex_change(struct fbotex *fbo, struct ra *ra, struct mp_log *log,
                   int w, int h, const struct ra_format *fmt, int flags);
#define FBOTEX_FUZZY_W 1
#define FBOTEX_FUZZY_H 2
#define FBOTEX_FUZZY (FBOTEX_FUZZY_W | FBOTEX_FUZZY_H)
