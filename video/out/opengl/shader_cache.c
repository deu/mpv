#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

#include <libavutil/sha.h>
#include <libavutil/mem.h>

#include "osdep/io.h"

#include "common/common.h"
#include "options/path.h"
#include "stream/stream.h"
#include "shader_cache.h"
#include "formats.h"
#include "utils.h"

// Force cache flush if more than this number of shaders is created.
#define SC_MAX_ENTRIES 48

union uniform_val {
    float f[9];         // RA_VARTYPE_FLOAT
    int i[4];           // RA_VARTYPE_INT
    struct ra_tex *tex; // RA_VARTYPE_TEX, RA_VARTYPE_IMG_*
    struct ra_buf *buf; // RA_VARTYPE_BUF_*
};

struct sc_uniform {
    struct ra_renderpass_input input;
    const char *glsl_type;
    union uniform_val v;
    char *buffer_format;
};

struct sc_cached_uniform {
    union uniform_val v;
};

struct sc_entry {
    struct ra_renderpass *pass;
    struct sc_cached_uniform *cached_uniforms;
    int num_cached_uniforms;
    bstr total;
    struct timer_pool *timer;
};

struct gl_shader_cache {
    struct ra *ra;
    struct mp_log *log;

    // permanent
    char **exts;
    int num_exts;

    // this is modified during use (gl_sc_add() etc.) and reset for each shader
    bstr prelude_text;
    bstr header_text;
    bstr text;
    int next_texture_unit;
    int next_image_unit;
    int next_buffer_binding;

    struct ra_renderpass_params params;

    struct sc_entry **entries;
    int num_entries;

    struct sc_entry *current_shader; // set by gl_sc_generate()

    struct sc_uniform *uniforms;
    int num_uniforms;

    struct ra_renderpass_input_val *values;
    int num_values;

    // For checking that the user is calling gl_sc_reset() properly.
    bool needs_reset;

    bool error_state; // true if an error occurred

    // temporary buffers (avoids frequent reallocations)
    bstr tmp[6];

    // For the disk-cache.
    char *cache_dir;
    struct mpv_global *global; // can be NULL
};

static void gl_sc_reset(struct gl_shader_cache *sc);

struct gl_shader_cache *gl_sc_create(struct ra *ra, struct mpv_global *global,
                                     struct mp_log *log)
{
    struct gl_shader_cache *sc = talloc_ptrtype(NULL, sc);
    *sc = (struct gl_shader_cache){
        .ra = ra,
        .global = global,
        .log = log,
    };
    gl_sc_reset(sc);
    return sc;
}

// Reset the previous pass. This must be called after gl_sc_generate and before
// starting a new shader.
static void gl_sc_reset(struct gl_shader_cache *sc)
{
    sc->prelude_text.len = 0;
    sc->header_text.len = 0;
    sc->text.len = 0;
    for (int n = 0; n < sc->num_uniforms; n++)
        talloc_free((void *)sc->uniforms[n].input.name);
    sc->num_uniforms = 0;
    sc->next_texture_unit = 1; // not 0, as 0 is "free for use"
    sc->next_image_unit = 1;
    sc->next_buffer_binding = 1;
    sc->current_shader = NULL;
    sc->params = (struct ra_renderpass_params){0};
    sc->needs_reset = false;
}

static void sc_flush_cache(struct gl_shader_cache *sc)
{
    MP_VERBOSE(sc, "flushing shader cache\n");

    for (int n = 0; n < sc->num_entries; n++) {
        struct sc_entry *e = sc->entries[n];
        if (e->pass)
            sc->ra->fns->renderpass_destroy(sc->ra, e->pass);
        timer_pool_destroy(e->timer);
        talloc_free(e);
    }
    sc->num_entries = 0;
}

void gl_sc_destroy(struct gl_shader_cache *sc)
{
    if (!sc)
        return;
    gl_sc_reset(sc);
    sc_flush_cache(sc);
    talloc_free(sc);
}

bool gl_sc_error_state(struct gl_shader_cache *sc)
{
    return sc->error_state;
}

void gl_sc_reset_error(struct gl_shader_cache *sc)
{
    sc->error_state = false;
}

void gl_sc_enable_extension(struct gl_shader_cache *sc, char *name)
{
    for (int n = 0; n < sc->num_exts; n++) {
        if (strcmp(sc->exts[n], name) == 0)
            return;
    }
    MP_TARRAY_APPEND(sc, sc->exts, sc->num_exts, talloc_strdup(sc, name));
}

#define bstr_xappend0(sc, b, s) bstr_xappend(sc, b, bstr0(s))

void gl_sc_add(struct gl_shader_cache *sc, const char *text)
{
    bstr_xappend0(sc, &sc->text, text);
}

void gl_sc_addf(struct gl_shader_cache *sc, const char *textf, ...)
{
    va_list ap;
    va_start(ap, textf);
    bstr_xappend_vasprintf(sc, &sc->text, textf, ap);
    va_end(ap);
}

void gl_sc_hadd(struct gl_shader_cache *sc, const char *text)
{
    bstr_xappend0(sc, &sc->header_text, text);
}

void gl_sc_haddf(struct gl_shader_cache *sc, const char *textf, ...)
{
    va_list ap;
    va_start(ap, textf);
    bstr_xappend_vasprintf(sc, &sc->header_text, textf, ap);
    va_end(ap);
}

void gl_sc_hadd_bstr(struct gl_shader_cache *sc, struct bstr text)
{
    bstr_xappend(sc, &sc->header_text, text);
}

void gl_sc_paddf(struct gl_shader_cache *sc, const char *textf, ...)
{
    va_list ap;
    va_start(ap, textf);
    bstr_xappend_vasprintf(sc, &sc->prelude_text, textf, ap);
    va_end(ap);
}

static struct sc_uniform *find_uniform(struct gl_shader_cache *sc,
                                       const char *name)
{
    struct sc_uniform new = {
        .input = {
            .dim_v = 1,
            .dim_m = 1,
        },
    };

    for (int n = 0; n < sc->num_uniforms; n++) {
        struct sc_uniform *u = &sc->uniforms[n];
        if (strcmp(u->input.name, name) == 0) {
            const char *allocname = u->input.name;
            *u = new;
            u->input.name = allocname;
            return u;
        }
    }

    // not found -> add it
    new.input.name = talloc_strdup(NULL, name);
    MP_TARRAY_APPEND(sc, sc->uniforms, sc->num_uniforms, new);
    return &sc->uniforms[sc->num_uniforms - 1];
}

void gl_sc_uniform_texture(struct gl_shader_cache *sc, char *name,
                           struct ra_tex *tex)
{
    const char *glsl_type = "sampler2D";
    if (tex->params.dimensions == 1) {
        glsl_type = "sampler1D";
    } else if (tex->params.dimensions == 3) {
        glsl_type = "sampler3D";
    } else if (tex->params.non_normalized) {
        glsl_type = "sampler2DRect";
    } else if (tex->params.external_oes) {
        glsl_type = "samplerExternalOES";
    } else if (tex->params.format->ctype == RA_CTYPE_UINT) {
        glsl_type = sc->ra->glsl_es ? "highp usampler2D" : "usampler2D";
    }

    struct sc_uniform *u = find_uniform(sc, name);
    u->input.type = RA_VARTYPE_TEX;
    u->glsl_type = glsl_type;
    u->input.binding = sc->next_texture_unit++;
    u->v.tex = tex;
}

void gl_sc_uniform_image2D_wo(struct gl_shader_cache *sc, const char *name,
                              struct ra_tex *tex)
{
    gl_sc_enable_extension(sc, "GL_ARB_shader_image_load_store");

    struct sc_uniform *u = find_uniform(sc, name);
    u->input.type = RA_VARTYPE_IMG_W;
    u->glsl_type = "writeonly image2D";
    u->input.binding = sc->next_image_unit++;
    u->v.tex = tex;
}

void gl_sc_ssbo(struct gl_shader_cache *sc, char *name, struct ra_buf *buf,
                char *format, ...)
{
    gl_sc_enable_extension(sc, "GL_ARB_shader_storage_buffer_object");

    struct sc_uniform *u = find_uniform(sc, name);
    u->input.type = RA_VARTYPE_BUF_RW;
    u->glsl_type = "";
    u->input.binding = sc->next_buffer_binding++;
    u->v.buf = buf;

    va_list ap;
    va_start(ap, format);
    u->buffer_format = ta_vasprintf(sc, format, ap);
    va_end(ap);
}

void gl_sc_uniform_f(struct gl_shader_cache *sc, char *name, float f)
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->input.type = RA_VARTYPE_FLOAT;
    u->glsl_type = "float";
    u->v.f[0] = f;
}

void gl_sc_uniform_i(struct gl_shader_cache *sc, char *name, int i)
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->input.type = RA_VARTYPE_INT;
    u->glsl_type = "int";
    u->v.i[0] = i;
}

void gl_sc_uniform_vec2(struct gl_shader_cache *sc, char *name, float f[2])
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->input.type = RA_VARTYPE_FLOAT;
    u->input.dim_v = 2;
    u->glsl_type = "vec2";
    u->v.f[0] = f[0];
    u->v.f[1] = f[1];
}

void gl_sc_uniform_vec3(struct gl_shader_cache *sc, char *name, GLfloat f[3])
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->input.type = RA_VARTYPE_FLOAT;
    u->input.dim_v = 3;
    u->glsl_type = "vec3";
    u->v.f[0] = f[0];
    u->v.f[1] = f[1];
    u->v.f[2] = f[2];
}

static void transpose2x2(float r[2 * 2])
{
    MPSWAP(float, r[0+2*1], r[1+2*0]);
}

void gl_sc_uniform_mat2(struct gl_shader_cache *sc, char *name,
                        bool transpose, GLfloat *v)
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->input.type = RA_VARTYPE_FLOAT;
    u->input.dim_v = 2;
    u->input.dim_m = 2;
    u->glsl_type = "mat2";
    for (int n = 0; n < 4; n++)
        u->v.f[n] = v[n];
    if (transpose)
        transpose2x2(&u->v.f[0]);
}

static void transpose3x3(float r[3 * 3])
{
    MPSWAP(float, r[0+3*1], r[1+3*0]);
    MPSWAP(float, r[0+3*2], r[2+3*0]);
    MPSWAP(float, r[1+3*2], r[2+3*1]);
}

void gl_sc_uniform_mat3(struct gl_shader_cache *sc, char *name,
                        bool transpose, GLfloat *v)
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->input.type = RA_VARTYPE_FLOAT;
    u->input.dim_v = 3;
    u->input.dim_m = 3;
    u->glsl_type = "mat3";
    for (int n = 0; n < 9; n++)
        u->v.f[n] = v[n];
    if (transpose)
        transpose3x3(&u->v.f[0]);
}

// Tell the shader generator (and later gl_sc_draw_data()) about the vertex
// data layout and attribute names. The entries array is terminated with a {0}
// entry. The array memory must remain valid indefinitely (for now).
void gl_sc_set_vertex_format(struct gl_shader_cache *sc,
                             const struct ra_renderpass_input *entries,
                             int vertex_stride)
{
    sc->params.vertex_attribs = (struct ra_renderpass_input *)entries;
    sc->params.num_vertex_attribs = 0;
    while (entries[sc->params.num_vertex_attribs].name)
        sc->params.num_vertex_attribs++;
    sc->params.vertex_stride = vertex_stride;
}

void gl_sc_blend(struct gl_shader_cache *sc,
                 enum ra_blend blend_src_rgb,
                 enum ra_blend blend_dst_rgb,
                 enum ra_blend blend_src_alpha,
                 enum ra_blend blend_dst_alpha)
{
    sc->params.enable_blend = true;
    sc->params.blend_src_rgb = blend_src_rgb;
    sc->params.blend_dst_rgb = blend_dst_rgb;
    sc->params.blend_src_alpha = blend_src_alpha;
    sc->params.blend_dst_alpha = blend_dst_alpha;
}

static const char *vao_glsl_type(const struct ra_renderpass_input *e)
{
    // pretty dumb... too dumb, but works for us
    switch (e->dim_v) {
    case 1: return "float";
    case 2: return "vec2";
    case 3: return "vec3";
    case 4: return "vec4";
    default: abort();
    }
}

static void update_uniform(struct gl_shader_cache *sc, struct sc_entry *e,
                           struct sc_uniform *u, int n)
{
    struct sc_cached_uniform *un = &e->cached_uniforms[n];
    struct ra_renderpass_input *input = &e->pass->params.inputs[n];
    size_t size = ra_render_pass_input_data_size(input);
    bool changed = true;
    if (size > 0)
        changed = memcmp(&un->v, &u->v, size) != 0;

    if (changed) {
        un->v = u->v;
        struct ra_renderpass_input_val value = {
            .index = n,
            .data = &un->v,
        };
        MP_TARRAY_APPEND(sc, sc->values, sc->num_values, value);
    }
}

void gl_sc_set_cache_dir(struct gl_shader_cache *sc, const char *dir)
{
    talloc_free(sc->cache_dir);
    sc->cache_dir = talloc_strdup(sc, dir);
}

static void create_pass(struct gl_shader_cache *sc, struct sc_entry *entry)
{
    void *tmp = talloc_new(NULL);
    struct ra_renderpass_params params = sc->params;

    MP_VERBOSE(sc, "new shader program:\n");
    if (sc->header_text.len) {
        MP_VERBOSE(sc, "header:\n");
        mp_log_source(sc->log, MSGL_V, sc->header_text.start);
        MP_VERBOSE(sc, "body:\n");
    }
    if (sc->text.len)
        mp_log_source(sc->log, MSGL_V, sc->text.start);

    // The vertex shader uses mangled names for the vertex attributes, so that
    // the fragment shader can use the "real" names. But the shader is expecting
    // the vertex attribute names (at least with older GLSL targets for GL).
    params.vertex_attribs = talloc_memdup(tmp, params.vertex_attribs,
                params.num_vertex_attribs * sizeof(params.vertex_attribs[0]));
    for (int n = 0; n < params.num_vertex_attribs; n++) {
        struct ra_renderpass_input *attrib = &params.vertex_attribs[n];
        attrib->name = talloc_asprintf(tmp, "vertex_%s", attrib->name);
    }

    const char *cache_header = "mpv shader cache v1\n";
    char *cache_filename = NULL;
    char *cache_dir = NULL;

    if (sc->cache_dir && sc->cache_dir[0]) {
        // Try to load it from a disk cache.
        cache_dir = mp_get_user_path(tmp, sc->global, sc->cache_dir);

        struct AVSHA *sha = av_sha_alloc();
        if (!sha)
            abort();
        av_sha_init(sha, 256);
        av_sha_update(sha, entry->total.start, entry->total.len);

        uint8_t hash[256 / 8];
        av_sha_final(sha, hash);
        av_free(sha);

        char hashstr[256 / 8 * 2 + 1];
        for (int n = 0; n < 256 / 8; n++)
            snprintf(hashstr + n * 2, sizeof(hashstr) - n * 2, "%02X", hash[n]);

        cache_filename = mp_path_join(tmp, cache_dir, hashstr);
        if (stat(cache_filename, &(struct stat){0}) == 0) {
            MP_VERBOSE(sc, "Trying to load shader from disk...\n");
            struct bstr cachedata =
                stream_read_file(cache_filename, tmp, sc->global, 1000000000);
            if (bstr_eatstart0(&cachedata, cache_header))
                params.cached_program = cachedata;
        }
    }

    entry->pass = sc->ra->fns->renderpass_create(sc->ra, &params);

    if (!entry->pass)
        sc->error_state = true;

    if (entry->pass && cache_filename) {
        bstr nc = entry->pass->params.cached_program;
        if (nc.len && !bstr_equals(params.cached_program, nc)) {
            mp_mkdirp(cache_dir);

            MP_VERBOSE(sc, "Writing shader cache file: %s\n", cache_filename);
            FILE *out = fopen(cache_filename, "wb");
            if (out) {
                fwrite(cache_header, strlen(cache_header), 1, out);
                fwrite(nc.start, nc.len, 1, out);
                fclose(out);
            }
        }
    }

    talloc_free(tmp);
}

#define ADD(x, ...) bstr_xappend_asprintf(sc, (x), __VA_ARGS__)
#define ADD_BSTR(x, s) bstr_xappend(sc, (x), (s))

static void add_uniforms(struct gl_shader_cache *sc, bstr *dst)
{
    for (int n = 0; n < sc->num_uniforms; n++) {
        struct sc_uniform *u = &sc->uniforms[n];
        switch (u->input.type) {
        case RA_VARTYPE_INT:
        case RA_VARTYPE_FLOAT:
        case RA_VARTYPE_TEX:
        case RA_VARTYPE_IMG_W:
            ADD(dst, "uniform %s %s;\n", u->glsl_type, u->input.name);
            break;
        case RA_VARTYPE_BUF_RW:
            ADD(dst, "layout(std430, binding=%d) buffer %s { %s };\n",
                u->input.binding, u->input.name, u->buffer_format);
            break;
        default: abort();
        }
    }
}

// 1. Generate vertex and fragment shaders from the fragment shader text added
//    with gl_sc_add(). The generated shader program is cached (based on the
//    text), so actual compilation happens only the first time.
// 2. Update the uniforms and textures set with gl_sc_uniform_*.
// 3. Make the new shader program current (glUseProgram()).
// After that, you render, and then you call gc_sc_reset(), which does:
// 1. Unbind the program and all textures.
// 2. Reset the sc state and prepare for a new shader program. (All uniforms
//    and fragment operations needed for the next program have to be re-added.)
static void gl_sc_generate(struct gl_shader_cache *sc, enum ra_renderpass_type type)
{
    int glsl_version = sc->ra->glsl_version;
    int glsl_es = sc->ra->glsl_es ? glsl_version : 0;

    sc->params.type = type;

    // gl_sc_reset() must be called after ending the previous render process,
    // and before starting a new one.
    assert(!sc->needs_reset);
    sc->needs_reset = true;

    // gl_sc_set_vertex_format() must always be called
    assert(sc->params.vertex_attribs);

    for (int n = 0; n < MP_ARRAY_SIZE(sc->tmp); n++)
        sc->tmp[n].len = 0;

    // set up shader text (header + uniforms + body)
    bstr *header = &sc->tmp[0];
    ADD(header, "#version %d%s\n", glsl_version, glsl_es >= 300 ? " es" : "");
    if (type == RA_RENDERPASS_TYPE_COMPUTE) {
        // This extension cannot be enabled in fragment shader. Enable it as
        // an exception for compute shader.
        ADD(header, "#extension GL_ARB_compute_shader : enable\n");
    }
    for (int n = 0; n < sc->num_exts; n++)
        ADD(header, "#extension %s : enable\n", sc->exts[n]);
    if (glsl_es) {
        ADD(header, "precision mediump float;\n");
        ADD(header, "precision mediump sampler2D;\n");
        if (sc->ra->caps & RA_CAP_TEX_3D)
            ADD(header, "precision mediump sampler3D;\n");
    }

    if (glsl_version >= 130) {
        ADD(header, "#define texture1D texture\n");
        ADD(header, "#define texture3D texture\n");
    } else {
        ADD(header, "#define texture texture2D\n");
    }

    // Additional helpers.
    ADD(header, "#define LUT_POS(x, lut_size)"
                " mix(0.5 / (lut_size), 1.0 - 0.5 / (lut_size), (x))\n");

    char *vert_in = glsl_version >= 130 ? "in" : "attribute";
    char *vert_out = glsl_version >= 130 ? "out" : "varying";
    char *frag_in = glsl_version >= 130 ? "in" : "varying";

    struct bstr *vert = NULL, *frag = NULL, *comp = NULL;

    if (type == RA_RENDERPASS_TYPE_RASTER) {
        // vertex shader: we don't use the vertex shader, so just setup a
        // dummy, which passes through the vertex array attributes.
        bstr *vert_head = &sc->tmp[1];
        ADD_BSTR(vert_head, *header);
        bstr *vert_body = &sc->tmp[2];
        ADD(vert_body, "void main() {\n");
        bstr *frag_vaos = &sc->tmp[3];
        for (int n = 0; n < sc->params.num_vertex_attribs; n++) {
            const struct ra_renderpass_input *e = &sc->params.vertex_attribs[n];
            const char *glsl_type = vao_glsl_type(e);
            if (strcmp(e->name, "position") == 0) {
                // setting raster pos. requires setting gl_Position magic variable
                assert(e->dim_v == 2 && e->type == RA_VARTYPE_FLOAT);
                ADD(vert_head, "%s vec2 vertex_position;\n", vert_in);
                ADD(vert_body, "gl_Position = vec4(vertex_position, 1.0, 1.0);\n");
            } else {
                ADD(vert_head, "%s %s vertex_%s;\n", vert_in, glsl_type, e->name);
                ADD(vert_head, "%s %s %s;\n", vert_out, glsl_type, e->name);
                ADD(vert_body, "%s = vertex_%s;\n", e->name, e->name);
                ADD(frag_vaos, "%s %s %s;\n", frag_in, glsl_type, e->name);
            }
        }
        ADD(vert_body, "}\n");
        vert = vert_head;
        ADD_BSTR(vert, *vert_body);

        // fragment shader; still requires adding used uniforms and VAO elements
        frag = &sc->tmp[4];
        ADD_BSTR(frag, *header);
        if (glsl_version >= 130)
            ADD(frag, "out vec4 out_color;\n");
        ADD_BSTR(frag, *frag_vaos);
        add_uniforms(sc, frag);

        ADD_BSTR(frag, sc->prelude_text);
        ADD_BSTR(frag, sc->header_text);

        ADD(frag, "void main() {\n");
        // we require _all_ frag shaders to write to a "vec4 color"
        ADD(frag, "vec4 color = vec4(0.0, 0.0, 0.0, 1.0);\n");
        ADD_BSTR(frag, sc->text);
        if (glsl_version >= 130) {
            ADD(frag, "out_color = color;\n");
        } else {
            ADD(frag, "gl_FragColor = color;\n");
        }
        ADD(frag, "}\n");
    }

    if (type == RA_RENDERPASS_TYPE_COMPUTE) {
        comp = &sc->tmp[4];
        ADD_BSTR(comp, *header);

        add_uniforms(sc, comp);

        ADD_BSTR(comp, sc->prelude_text);
        ADD_BSTR(comp, sc->header_text);

        ADD(comp, "void main() {\n");
        ADD(comp, "vec4 color = vec4(0.0, 0.0, 0.0, 1.0);\n"); // convenience
        ADD_BSTR(comp, sc->text);
        ADD(comp, "}\n");
    }

    bstr *hash_total = &sc->tmp[5];

    ADD(hash_total, "type %d\n", sc->params.type);

    if (frag) {
        ADD_BSTR(hash_total, *frag);
        sc->params.frag_shader = frag->start;
    }
    ADD(hash_total, "\n");
    if (vert) {
        ADD_BSTR(hash_total, *vert);
        sc->params.vertex_shader = vert->start;
    }
    ADD(hash_total, "\n");
    if (comp) {
        ADD_BSTR(hash_total, *comp);
        sc->params.compute_shader = comp->start;
    }
    ADD(hash_total, "\n");

    if (sc->params.enable_blend) {
        ADD(hash_total, "blend %d %d %d %d\n",
            sc->params.blend_src_rgb, sc->params.blend_dst_rgb,
            sc->params.blend_src_alpha, sc->params.blend_dst_alpha);
    }

    struct sc_entry *entry = NULL;
    for (int n = 0; n < sc->num_entries; n++) {
        struct sc_entry *cur = sc->entries[n];
        if (bstr_equals(cur->total, *hash_total)) {
            entry = cur;
            break;
        }
    }
    if (!entry) {
        if (sc->num_entries == SC_MAX_ENTRIES)
            sc_flush_cache(sc);
        entry = talloc_ptrtype(NULL, entry);
        *entry = (struct sc_entry){
            .total = bstrdup(entry, *hash_total),
            .timer = timer_pool_create(sc->ra),
        };
        for (int n = 0; n < sc->num_uniforms; n++) {
            struct sc_cached_uniform u = {0};
            MP_TARRAY_APPEND(entry, entry->cached_uniforms,
                             entry->num_cached_uniforms, u);
            MP_TARRAY_APPEND(sc, sc->params.inputs, sc->params.num_inputs,
                             sc->uniforms[n].input);
        }
        create_pass(sc, entry);
        MP_TARRAY_APPEND(sc, sc->entries, sc->num_entries, entry);
    }
    if (!entry->pass)
        return;

    assert(sc->num_uniforms == entry->num_cached_uniforms);
    assert(sc->num_uniforms == entry->pass->params.num_inputs);

    sc->num_values = 0;
    for (int n = 0; n < sc->num_uniforms; n++)
        update_uniform(sc, entry, &sc->uniforms[n], n);

    sc->current_shader = entry;
}

struct mp_pass_perf gl_sc_dispatch_draw(struct gl_shader_cache *sc,
                                        struct ra_tex *target,
                                        void *ptr, size_t num)
{
    struct timer_pool *timer = NULL;

    gl_sc_generate(sc, RA_RENDERPASS_TYPE_RASTER);
    if (!sc->current_shader)
        goto error;

    timer = sc->current_shader->timer;

    struct mp_rect full_rc = {0, 0, target->params.w, target->params.h};

    struct ra_renderpass_run_params run = {
        .pass = sc->current_shader->pass,
        .values = sc->values,
        .num_values = sc->num_values,
        .target = target,
        .vertex_data = ptr,
        .vertex_count = num,
        .viewport = full_rc,
        .scissors = full_rc,
    };

    timer_pool_start(timer);
    sc->ra->fns->renderpass_run(sc->ra, &run);
    timer_pool_stop(timer);

error:
    gl_sc_reset(sc);
    return timer_pool_measure(timer);
}

struct mp_pass_perf gl_sc_dispatch_compute(struct gl_shader_cache *sc,
                                           int w, int h, int d)
{
    struct timer_pool *timer = NULL;

    gl_sc_generate(sc, RA_RENDERPASS_TYPE_COMPUTE);
    if (!sc->current_shader)
        goto error;

    timer = sc->current_shader->timer;

    struct ra_renderpass_run_params run = {
        .pass = sc->current_shader->pass,
        .values = sc->values,
        .num_values = sc->num_values,
        .compute_groups = {w, h, d},
    };

    timer_pool_start(timer);
    sc->ra->fns->renderpass_run(sc->ra, &run);
    timer_pool_stop(timer);

error:
    gl_sc_reset(sc);
    return timer_pool_measure(timer);
}
