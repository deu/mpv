#ifndef MP_GL_LCMS_H
#define MP_GL_LCMS_H

extern const struct m_sub_options mp_icc_conf;

struct mp_icc_opts {
    char *profile;
    int profile_auto_flag;
    char *profile_auto;
    char *cache;
    char *size_str;
    int intent;
};

struct lut3d;
struct mp_log;
struct mpv_global;
struct lut3d *mp_load_icc(struct mp_icc_opts *opts, struct mp_log *log,
                          struct mpv_global *global);

#endif
