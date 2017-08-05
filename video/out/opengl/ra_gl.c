#include <libavutil/intreadwrite.h>

#include "formats.h"
#include "ra_gl.h"

static struct ra_fns ra_fns_gl;

int ra_init_gl(struct ra *ra, GL *gl)
{
    if (gl->version < 210 && gl->es < 200) {
        MP_ERR(ra, "At least OpenGL 2.1 or OpenGL ES 2.0 required.\n");
        return -1;
    }

    struct ra_gl *p = ra->priv = talloc_zero(NULL, struct ra_gl);
    p->gl = gl;

    ra->fns = &ra_fns_gl;
    ra->caps = 0;
    if (gl->mpgl_caps & MPGL_CAP_1D_TEX)
        ra->caps |= RA_CAP_TEX_1D;
    if (gl->mpgl_caps & MPGL_CAP_3D_TEX)
        ra->caps |= RA_CAP_TEX_3D;
    if (gl->BlitFramebuffer)
        ra->caps |= RA_CAP_BLIT;
    if (gl->mpgl_caps & MPGL_CAP_COMPUTE_SHADER)
        ra->caps |= RA_CAP_COMPUTE;
    if (gl->MapBufferRange)
        ra->caps |= RA_CAP_PBO;
    if (gl->mpgl_caps & MPGL_CAP_NESTED_ARRAY)
        ra->caps |= RA_CAP_NESTED_ARRAY;
    ra->glsl_version = gl->glsl_version;
    ra->glsl_es = gl->es > 0;

    int gl_fmt_features = gl_format_feature_flags(gl);

    // Test whether we can use 10 bit.
    int depth16 = gl_determine_16bit_tex_depth(gl);
    MP_VERBOSE(ra, "16 bit texture depth: %d.\n", depth16);

    for (int n = 0; gl_formats[n].internal_format; n++) {
        const struct gl_format *gl_fmt = &gl_formats[n];

        if (!(gl_fmt->flags & gl_fmt_features))
            continue;

        struct ra_format *fmt = talloc_zero(ra, struct ra_format);
        *fmt = (struct ra_format){
            .name           = gl_fmt->name,
            .priv           = (void *)gl_fmt,
            .ctype          = gl_format_type(gl_fmt),
            .num_components = gl_format_components(gl_fmt->format),
            .pixel_size     = gl_bytes_per_pixel(gl_fmt->format, gl_fmt->type),
            .luminance_alpha = gl_fmt->format == GL_LUMINANCE_ALPHA,
            .linear_filter  = gl_fmt->flags & F_TF,
            .renderable     = (gl_fmt->flags & F_CR) &&
                              (gl->mpgl_caps & MPGL_CAP_FB),
        };

        int csize = gl_component_size(gl_fmt->type) * 8;
        int depth = csize;
        if (fmt->ctype == RA_CTYPE_UNORM)
            depth = MPMIN(csize, depth16); // naive/approximate
        if (gl_fmt->flags & F_F16) {
            depth = 16;
            csize = 32; // always upload as GL_FLOAT (simpler for us)
        }

        for (int i = 0; i < fmt->num_components; i++) {
            fmt->component_size[i] = csize;
            fmt->component_depth[i] = depth;
        }

        // Special formats for which OpenGL happens to have direct support.
        if (strcmp(fmt->name, "rgb565") == 0) {
            fmt->special_imgfmt = IMGFMT_RGB565;
            struct ra_imgfmt_desc *desc = talloc_zero(fmt, struct ra_imgfmt_desc);
            fmt->special_imgfmt_desc = desc;
            desc->num_planes = 1;
            desc->planes[0] = fmt;
            for (int i = 0; i < 3; i++)
                desc->components[0][i] = i + 1;
            desc->chroma_w = desc->chroma_h = 1;
        }
        if (strcmp(fmt->name, "appleyp") == 0) {
            fmt->special_imgfmt = IMGFMT_UYVY;
            struct ra_imgfmt_desc *desc = talloc_zero(fmt, struct ra_imgfmt_desc);
            fmt->special_imgfmt_desc = desc;
            desc->num_planes = 1;
            desc->planes[0] = fmt;
            desc->components[0][0] = 3;
            desc->components[0][1] = 1;
            desc->components[0][2] = 2;
            desc->chroma_w = desc->chroma_h = 1;
        }

        MP_TARRAY_APPEND(ra, ra->formats, ra->num_formats, fmt);
    }

    GLint max_wh;
    gl->GetIntegerv(GL_MAX_TEXTURE_SIZE, &max_wh);
    ra->max_texture_wh = max_wh;

    gl->Disable(GL_DITHER);

    return 0;
}

static void gl_destroy(struct ra *ra)
{
    talloc_free(ra->priv);
}

static void gl_tex_destroy(struct ra *ra, struct ra_tex *tex)
{
    struct ra_gl *p = ra->priv;
    struct ra_tex_gl *tex_gl = tex->priv;

    if (tex_gl->own_objects) {
        if (tex_gl->fbo)
            p->gl->DeleteFramebuffers(1, &tex_gl->fbo);

        p->gl->DeleteTextures(1, &tex_gl->texture);
    }
    gl_pbo_upload_uninit(&tex_gl->pbo);
    talloc_free(tex_gl);
    talloc_free(tex);
}

static struct ra_tex *gl_tex_create(struct ra *ra,
                                    const struct ra_tex_params *params)
{
    struct ra_gl *p = ra->priv;
    GL *gl = p->gl;

    struct ra_tex *tex = talloc_zero(NULL, struct ra_tex);
    tex->params = *params;
    struct ra_tex_gl *tex_gl = tex->priv = talloc_zero(NULL, struct ra_tex_gl);

    const struct gl_format *fmt = params->format->priv;
    tex_gl->own_objects = true;
    tex_gl->internal_format = fmt->internal_format;
    tex_gl->format = fmt->format;
    tex_gl->type = fmt->type;
    switch (params->dimensions) {
    case 1: tex_gl->target = GL_TEXTURE_1D; break;
    case 2: tex_gl->target = GL_TEXTURE_2D; break;
    case 3: tex_gl->target = GL_TEXTURE_3D; break;
    default: abort();
    }
    if (params->non_normalized) {
        assert(params->dimensions == 2);
        tex_gl->target = GL_TEXTURE_RECTANGLE;
    }

    gl->GenTextures(1, &tex_gl->texture);
    gl->BindTexture(tex_gl->target, tex_gl->texture);

    GLint filter = params->src_linear ? GL_LINEAR : GL_NEAREST;
    GLint wrap = params->src_repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    gl->TexParameteri(tex_gl->target, GL_TEXTURE_MIN_FILTER, filter);
    gl->TexParameteri(tex_gl->target, GL_TEXTURE_MAG_FILTER, filter);
    gl->TexParameteri(tex_gl->target, GL_TEXTURE_WRAP_S, wrap);
    if (params->dimensions > 1)
        gl->TexParameteri(tex_gl->target, GL_TEXTURE_WRAP_T, wrap);
    if (params->dimensions > 2)
        gl->TexParameteri(tex_gl->target, GL_TEXTURE_WRAP_R, wrap);

    gl->PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    switch (params->dimensions) {
    case 1:
        gl->TexImage1D(tex_gl->target, 0, tex_gl->internal_format, params->w,
                       0, tex_gl->format, tex_gl->type, params->initial_data);
        break;
    case 2:
        gl->TexImage2D(tex_gl->target, 0, tex_gl->internal_format, params->w,
                       params->h, 0, tex_gl->format, tex_gl->type,
                       params->initial_data);
        break;
    case 3:
        gl->TexImage3D(tex_gl->target, 0, tex_gl->internal_format, params->w,
                       params->h, params->d, 0, tex_gl->format, tex_gl->type,
                       params->initial_data);
        break;
    }
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, 4);

    gl->BindTexture(tex_gl->target, 0);

    tex->params.initial_data = NULL;

    gl_check_error(gl, ra->log, "after creating texture");

    if (tex->params.render_dst) {
        if (!tex->params.format->renderable) {
            MP_ERR(ra, "Trying to create renderable texture with unsupported "
                   "format.\n");
            ra_tex_free(ra, &tex);
            return NULL;
        }

        assert(gl->mpgl_caps & MPGL_CAP_FB);

        gl->GenFramebuffers(1, &tex_gl->fbo);
        gl->BindFramebuffer(GL_FRAMEBUFFER, tex_gl->fbo);
        gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, tex_gl->texture, 0);
        GLenum err = gl->CheckFramebufferStatus(GL_FRAMEBUFFER);
        gl->BindFramebuffer(GL_FRAMEBUFFER, 0);

        if (err != GL_FRAMEBUFFER_COMPLETE) {
            MP_ERR(ra, "Error: framebuffer completeness check failed (error=%d).\n",
                   (int)err);
            ra_tex_free(ra, &tex);
            return NULL;
        }


        gl_check_error(gl, ra->log, "after creating framebuffer");
    }

    return tex;
}

static const struct ra_format fbo_dummy_format = {
    .name = "unknown_fbo",
    .priv = (void *)&(const struct gl_format){
        .name = "unknown",
        .format = GL_RGBA,
        .flags = F_CR,
    },
    .renderable = true,
};

static const struct ra_format tex_dummy_format = {
    .name = "unknown_tex",
    .priv = (void *)&(const struct gl_format){
        .name = "unknown",
        .format = GL_RGBA,
        .flags = F_TF,
    },
    .renderable = true,
    .linear_filter = true,
};

static const struct ra_format *find_similar_format(struct ra *ra,
                                                   GLint gl_iformat,
                                                   GLenum gl_format,
                                                   GLenum gl_type)
{
    if (gl_iformat || gl_format || gl_type) {
        for (int n = 0; n < ra->num_formats; n++) {
            const struct ra_format *fmt = ra->formats[n];
            const struct gl_format *gl_fmt = fmt->priv;
            if ((gl_fmt->internal_format == gl_iformat || !gl_iformat) &&
                (gl_fmt->format == gl_format || !gl_format) &&
                (gl_fmt->type == gl_type || !gl_type))
                return fmt;
        }
    }
    return NULL;
}

static struct ra_tex *wrap_tex_fbo(struct ra *ra, GLuint gl_obj, bool is_fbo,
                                   GLenum gl_target, GLint gl_iformat,
                                   GLenum gl_format, GLenum gl_type,
                                   int w, int h)
{
    const struct ra_format *format =
        find_similar_format(ra, gl_iformat, gl_format, gl_type);
    if (!format)
        format = is_fbo ? &fbo_dummy_format : &tex_dummy_format;

    struct ra_tex *tex = talloc_zero(ra, struct ra_tex);
    *tex = (struct ra_tex){
        .params = {
            .dimensions = 2,
            .w = w, .h = h, .d = 1,
            .format = format,
            .render_dst = is_fbo,
            .render_src = !is_fbo,
            .non_normalized = gl_target == GL_TEXTURE_RECTANGLE,
            .external_oes = gl_target == GL_TEXTURE_EXTERNAL_OES,
        },
    };

    struct ra_tex_gl *tex_gl = tex->priv = talloc_zero(NULL, struct ra_tex_gl);
    *tex_gl = (struct ra_tex_gl){
        .target = gl_target,
        .texture = is_fbo ? 0 : gl_obj,
        .fbo = is_fbo ? gl_obj : 0,
        .internal_format = gl_iformat,
        .format = gl_format,
        .type = gl_type,
    };

    return tex;
}

// Create a ra_tex that merely wraps an existing texture. gl_format and gl_type
// can be 0, in which case possibly nonsensical fallbacks are chosen.
// Works for 2D textures only. Integer textures are not supported.
// The returned object is freed with ra_tex_free(), but this will not delete
// the texture passed to this function.
struct ra_tex *ra_create_wrapped_texture(struct ra *ra, GLuint gl_texture,
                                         GLenum gl_target, GLint gl_iformat,
                                         GLenum gl_format, GLenum gl_type,
                                         int w, int h)
{
    return wrap_tex_fbo(ra, gl_texture, false, gl_target, gl_iformat, gl_format,
                        gl_type, w, h);
}

// Create a ra_tex that merely wraps an existing framebuffer. gl_fbo can be 0
// to wrap the default framebuffer.
// The returned object is freed with ra_tex_free(), but this will not delete
// the framebuffer object passed to this function.
struct ra_tex *ra_create_wrapped_fb(struct ra *ra, GLuint gl_fbo, int w, int h)
{
    return wrap_tex_fbo(ra, gl_fbo, true, 0, GL_RGBA, 0, 0, w, h);
}

GL *ra_gl_get(struct ra *ra)
{
    struct ra_gl *p = ra->priv;
    return p->gl;
}

static void gl_tex_upload(struct ra *ra, struct ra_tex *tex,
                          const void *src, ptrdiff_t stride,
                          struct mp_rect *rc, uint64_t flags,
                          struct ra_mapped_buffer *buf)
{
    struct ra_gl *p = ra->priv;
    GL *gl = p->gl;
    struct ra_tex_gl *tex_gl = tex->priv;
    struct ra_mapped_buffer_gl *buf_gl = NULL;
    struct mp_rect full = {0, 0, tex->params.w, tex->params.h};

    if (buf) {
        buf_gl = buf->priv;
        gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, buf_gl->pbo);
        src = (void *)((uintptr_t)src - (uintptr_t)buf->data);
    }

    gl->BindTexture(tex_gl->target, tex_gl->texture);

    switch (tex->params.dimensions) {
    case 1:
        assert(!rc);
        gl->TexImage1D(tex_gl->target, 0, tex_gl->internal_format,
                       tex->params.w, 0, tex_gl->format, tex_gl->type, src);
        break;
    case 2:
        if (!rc)
            rc = &full;
        gl_pbo_upload_tex(&tex_gl->pbo, gl, ra->use_pbo && !buf,
                          tex_gl->target, tex_gl->format, tex_gl->type,
                          tex->params.w, tex->params.h, src, stride,
                          rc->x0, rc->y0, rc->x1 - rc->x0, rc->y1 - rc->y0);
        break;
    case 3:
        assert(!rc);
        gl->PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl->TexImage3D(GL_TEXTURE_3D, 0, tex_gl->internal_format, tex->params.w,
                       tex->params.h, tex->params.d, 0, tex_gl->format,
                       tex_gl->type, src);
        gl->PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        break;
    }

    gl->BindTexture(tex_gl->target, 0);

    if (buf) {
        gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        // Make sure the PBO is not reused until GL is done with it. If a
        // previous operation is pending, "update" it by creating a new
        // fence that will cover the previous operation as well.
        gl->DeleteSync(buf_gl->fence);
        buf_gl->fence = gl->FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
}

static void gl_destroy_mapped_buffer(struct ra *ra, struct ra_mapped_buffer *buf)
{
    struct ra_gl *p = ra->priv;
    GL *gl = p->gl;
    struct ra_mapped_buffer_gl *buf_gl = buf->priv;

    gl->DeleteSync(buf_gl->fence);
    gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, buf_gl->pbo);
    if (buf->data)
        gl->UnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    gl->DeleteBuffers(1, &buf_gl->pbo);

    talloc_free(buf_gl);
    talloc_free(buf);
}

static struct ra_mapped_buffer *gl_create_mapped_buffer(struct ra *ra,
                                                        size_t size)
{
    struct ra_gl *p = ra->priv;
    GL *gl = p->gl;

    if (gl->version < 440)
        return NULL;

    struct ra_mapped_buffer *buf = talloc_zero(NULL, struct ra_mapped_buffer);
    buf->size = size;

    struct ra_mapped_buffer_gl *buf_gl = buf->priv =
        talloc_zero(NULL, struct ra_mapped_buffer_gl);

    unsigned flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT |
                     GL_MAP_COHERENT_BIT;

    gl->GenBuffers(1, &buf_gl->pbo);
    gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, buf_gl->pbo);
    gl->BufferStorage(GL_PIXEL_UNPACK_BUFFER, size, NULL, flags | GL_CLIENT_STORAGE_BIT);
    buf->data = gl->MapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, buf->size, flags);
    gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (!buf->data) {
        gl_check_error(gl, ra->log, "mapping buffer");
        gl_destroy_mapped_buffer(ra, buf);
        return NULL;
    }

    return buf;
}

static bool gl_poll_mapped_buffer(struct ra *ra, struct ra_mapped_buffer *buf)
{
    struct ra_gl *p = ra->priv;
    GL *gl = p->gl;
    struct ra_mapped_buffer_gl *buf_gl = buf->priv;

    if (buf_gl->fence) {
        GLenum res = gl->ClientWaitSync(buf_gl->fence, 0, 0); // non-blocking
        if (res == GL_ALREADY_SIGNALED) {
            gl->DeleteSync(buf_gl->fence);
            buf_gl->fence = NULL;
        }
    }

    return !buf_gl->fence;
}

static void gl_clear(struct ra *ra, struct ra_tex *dst, float color[4],
                     struct mp_rect *scissor)
{
    struct ra_gl *p = ra->priv;
    GL *gl = p->gl;

    assert(dst->params.render_dst);
    struct ra_tex_gl *dst_gl = dst->priv;

    gl->BindFramebuffer(GL_FRAMEBUFFER, dst_gl->fbo);

    gl->Scissor(scissor->x0, scissor->y0,
                scissor->x1 - scissor->x0,
                scissor->y1 - scissor->y0);

    gl->Enable(GL_SCISSOR_TEST);
    gl->ClearColor(color[0], color[1], color[2], color[3]);
    gl->Clear(GL_COLOR_BUFFER_BIT);
    gl->Disable(GL_SCISSOR_TEST);

    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

static void gl_blit(struct ra *ra, struct ra_tex *dst, struct ra_tex *src,
                    int dst_x, int dst_y, struct mp_rect *src_rc)
{
    struct ra_gl *p = ra->priv;
    GL *gl = p->gl;

    assert(dst->params.render_dst);
    assert(src->params.render_dst); // even src must have a FBO

    struct ra_tex_gl *src_gl = src->priv;
    struct ra_tex_gl *dst_gl = dst->priv;

    int w = mp_rect_w(*src_rc);
    int h = mp_rect_h(*src_rc);

    gl->BindFramebuffer(GL_READ_FRAMEBUFFER, src_gl->fbo);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_gl->fbo);
    gl->BlitFramebuffer(src_rc->x0, src_rc->y0, src_rc->x1, src_rc->y1,
                        dst_x, dst_y, dst_x + w, dst_y + h,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl->BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

static void gl_renderpass_destroy(struct ra *ra, struct ra_renderpass *pass)
{
    struct ra_gl *p = ra->priv;
    struct ra_renderpass_gl *pass_gl = pass->priv;
    p->gl->DeleteProgram(pass_gl->program);
    gl_vao_uninit(&pass_gl->vao);

    talloc_free(pass_gl);
    talloc_free(pass);
}

static const char *shader_typestr(GLenum type)
{
    switch (type) {
    case GL_VERTEX_SHADER:   return "vertex";
    case GL_FRAGMENT_SHADER: return "fragment";
    case GL_COMPUTE_SHADER:  return "compute";
    default: abort();
    }
}

static void compile_attach_shader(struct ra *ra, GLuint program,
                                  GLenum type, const char *source, bool *ok)
{
    struct ra_gl *p = ra->priv;
    GL *gl = p->gl;

    GLuint shader = gl->CreateShader(type);
    gl->ShaderSource(shader, 1, &source, NULL);
    gl->CompileShader(shader);
    GLint status = 0;
    gl->GetShaderiv(shader, GL_COMPILE_STATUS, &status);
    GLint log_length = 0;
    gl->GetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);

    int pri = status ? (log_length > 1 ? MSGL_V : MSGL_DEBUG) : MSGL_ERR;
    const char *typestr = shader_typestr(type);
    if (mp_msg_test(ra->log, pri)) {
        MP_MSG(ra, pri, "%s shader source:\n", typestr);
        mp_log_source(ra->log, pri, source);
    }
    if (log_length > 1) {
        GLchar *logstr = talloc_zero_size(NULL, log_length + 1);
        gl->GetShaderInfoLog(shader, log_length, NULL, logstr);
        MP_MSG(ra, pri, "%s shader compile log (status=%d):\n%s\n",
               typestr, status, logstr);
        talloc_free(logstr);
    }
    if (gl->GetTranslatedShaderSourceANGLE && mp_msg_test(ra->log, MSGL_DEBUG)) {
        GLint len = 0;
        gl->GetShaderiv(shader, GL_TRANSLATED_SHADER_SOURCE_LENGTH_ANGLE, &len);
        if (len > 0) {
            GLchar *sstr = talloc_zero_size(NULL, len + 1);
            gl->GetTranslatedShaderSourceANGLE(shader, len, NULL, sstr);
            MP_DBG(ra, "Translated shader:\n");
            mp_log_source(ra->log, MSGL_DEBUG, sstr);
        }
    }

    gl->AttachShader(program, shader);
    gl->DeleteShader(shader);

    *ok &= status;
}

static void link_shader(struct ra *ra, GLuint program, bool *ok)
{
    struct ra_gl *p = ra->priv;
    GL *gl = p->gl;

    gl->LinkProgram(program);
    GLint status = 0;
    gl->GetProgramiv(program, GL_LINK_STATUS, &status);
    GLint log_length = 0;
    gl->GetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);

    int pri = status ? (log_length > 1 ? MSGL_V : MSGL_DEBUG) : MSGL_ERR;
    if (mp_msg_test(ra->log, pri)) {
        GLchar *logstr = talloc_zero_size(NULL, log_length + 1);
        gl->GetProgramInfoLog(program, log_length, NULL, logstr);
        MP_MSG(ra, pri, "shader link log (status=%d): %s\n", status, logstr);
        talloc_free(logstr);
    }

    *ok &= status;
}

// either 'compute' or both 'vertex' and 'frag' are needed
static GLuint compile_program(struct ra *ra, const struct ra_renderpass_params *p)
{
    struct ra_gl *priv = ra->priv;
    GL *gl = priv->gl;

    GLuint prog = gl->CreateProgram();
    bool ok = true;
    if (p->type == RA_RENDERPASS_TYPE_COMPUTE)
        compile_attach_shader(ra, prog, GL_COMPUTE_SHADER, p->compute_shader, &ok);
    if (p->type == RA_RENDERPASS_TYPE_RASTER) {
        compile_attach_shader(ra, prog, GL_VERTEX_SHADER, p->vertex_shader, &ok);
        compile_attach_shader(ra, prog, GL_FRAGMENT_SHADER, p->frag_shader, &ok);
        for (int n = 0; n < p->num_vertex_attribs; n++)
            gl->BindAttribLocation(prog, n, p->vertex_attribs[n].name);
    }
    link_shader(ra, prog, &ok);
    if (!ok) {
        gl->DeleteProgram(prog);
        prog = 0;
    }
    return prog;
}

static GLuint load_program(struct ra *ra, const struct ra_renderpass_params *p,
                           bstr *out_cached_data)
{
    struct ra_gl *priv = ra->priv;
    GL *gl = priv->gl;

    GLuint prog = 0;

    if (gl->ProgramBinary && p->cached_program.len > 4) {
        GLenum format = AV_RL32(p->cached_program.start);
        prog = gl->CreateProgram();
        gl_check_error(gl, ra->log, "before loading program");
        gl->ProgramBinary(prog, format, p->cached_program.start + 4,
                                        p->cached_program.len - 4);
        gl->GetError(); // discard potential useless error
        GLint status = 0;
        gl->GetProgramiv(prog, GL_LINK_STATUS, &status);
        if (status) {
            MP_VERBOSE(ra, "Loading binary program succeeded.\n");
        } else {
            gl->DeleteProgram(prog);
            prog = 0;
        }
    }

    if (!prog) {
        prog = compile_program(ra, p);

        if (gl->GetProgramBinary && prog) {
            GLint size = 0;
            gl->GetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &size);
            uint8_t *buffer = talloc_size(NULL, size + 4);
            GLsizei actual_size = 0;
            GLenum binary_format = 0;
            gl->GetProgramBinary(prog, size, &actual_size, &binary_format,
                                 buffer + 4);
            AV_WL32(buffer, binary_format);
            if (actual_size)
                *out_cached_data = (bstr){buffer, actual_size + 4};
        }
    }

    return prog;
}

static struct ra_renderpass *gl_renderpass_create(struct ra *ra,
                                    const struct ra_renderpass_params *params)
{
    struct ra_gl *p = ra->priv;
    GL *gl = p->gl;

    struct ra_renderpass *pass = talloc_zero(NULL, struct ra_renderpass);
    pass->params = *ra_render_pass_params_copy(pass, params);
    pass->params.cached_program = (bstr){0};
    struct ra_renderpass_gl *pass_gl = pass->priv =
        talloc_zero(NULL, struct ra_renderpass_gl);

    bstr cached = {0};
    pass_gl->program = load_program(ra, params, &cached);
    if (!pass_gl->program) {
        gl_renderpass_destroy(ra, pass);
        return NULL;
    }

    talloc_steal(pass, cached.start);
    pass->params.cached_program = cached;

    for (int n = 0; n < params->num_inputs; n++) {
        GLint loc =
            gl->GetUniformLocation(pass_gl->program, params->inputs[n].name);
        MP_TARRAY_APPEND(pass_gl, pass_gl->uniform_loc, pass_gl->num_uniform_loc,
                         loc);
    }

    gl_vao_init(&pass_gl->vao, gl, params->vertex_stride, params->vertex_attribs,
                params->num_vertex_attribs);

    pass_gl->first_run = true;

    return pass;
}

static GLenum map_blend(enum ra_blend blend)
{
    switch (blend) {
    case RA_BLEND_ZERO:                 return GL_ZERO;
    case RA_BLEND_ONE:                  return GL_ONE;
    case RA_BLEND_SRC_ALPHA:            return GL_SRC_ALPHA;
    case RA_BLEND_ONE_MINUS_SRC_ALPHA:  return GL_ONE_MINUS_SRC_ALPHA;
    default: return 0;
    }
}

// Assumes program is current (gl->UseProgram(program)).
static void update_uniform(struct ra *ra, struct ra_renderpass *pass,
                           struct ra_renderpass_input_val *val)
{
    struct ra_gl *p = ra->priv;
    GL *gl = p->gl;
    struct ra_renderpass_gl *pass_gl = pass->priv;

    struct ra_renderpass_input *input = &pass->params.inputs[val->index];
    assert(val->index >= 0 && val->index < pass_gl->num_uniform_loc);
    GLint loc = pass_gl->uniform_loc[val->index];

    switch (input->type) {
    case RA_VARTYPE_INT: {
        assert(input->dim_v * input->dim_m == 1);
        if (loc < 0)
            break;
        gl->Uniform1i(loc, *(int *)val->data);
        break;
    }
    case RA_VARTYPE_FLOAT: {
        float *f = val->data;
        if (loc < 0)
            break;
        if (input->dim_m == 1) {
            switch (input->dim_v) {
            case 1: gl->Uniform1f(loc, f[0]); break;
            case 2: gl->Uniform2f(loc, f[0], f[1]); break;
            case 3: gl->Uniform3f(loc, f[0], f[1], f[2]); break;
            case 4: gl->Uniform4f(loc, f[0], f[1], f[2], f[3]); break;
            default: abort();
            }
        } else if (input->dim_v == 2 && input->dim_m == 2) {
            gl->UniformMatrix2fv(loc, 1, GL_FALSE, f);
        } else if (input->dim_v == 3 && input->dim_m == 3) {
            gl->UniformMatrix3fv(loc, 1, GL_FALSE, f);
        } else {
            abort();
        }
        break;
    }
    case RA_VARTYPE_IMG_W: /* fall through */
    case RA_VARTYPE_TEX: {
        struct ra_tex *tex = *(struct ra_tex **)val->data;
        struct ra_tex_gl *tex_gl = tex->priv;
        assert(tex->params.render_src);
        if (pass_gl->first_run)
            gl->Uniform1i(loc, input->binding);
        if (input->type == RA_VARTYPE_TEX) {
            gl->ActiveTexture(GL_TEXTURE0 + input->binding);
            gl->BindTexture(tex_gl->target, tex_gl->texture);
        } else {
            gl->BindImageTexture(input->binding, tex_gl->texture, 0, GL_FALSE, 0,
                                 GL_WRITE_ONLY, tex_gl->internal_format);
        }
        break;
    }
    case RA_VARTYPE_SSBO: {
        gl->BindBufferBase(GL_SHADER_STORAGE_BUFFER, input->binding,
                           *(int *)val->data);
        break;
    }
    default:
        abort();
    }
}

static void disable_binding(struct ra *ra, struct ra_renderpass *pass,
                           struct ra_renderpass_input_val *val)
{
    struct ra_gl *p = ra->priv;
    GL *gl = p->gl;

    struct ra_renderpass_input *input = &pass->params.inputs[val->index];

    switch (input->type) {
    case RA_VARTYPE_IMG_W: /* fall  through */
    case RA_VARTYPE_TEX: {
        struct ra_tex *tex = *(struct ra_tex **)val->data;
        struct ra_tex_gl *tex_gl = tex->priv;
        assert(tex->params.render_src);
        if (input->type == RA_VARTYPE_TEX) {
            gl->ActiveTexture(GL_TEXTURE0 + input->binding);
            gl->BindTexture(tex_gl->target, 0);
        } else {
            gl->BindImageTexture(input->binding, 0, 0, GL_FALSE, 0,
                                 GL_WRITE_ONLY, tex_gl->internal_format);
        }
        break;
    }
    case RA_VARTYPE_SSBO: {
        gl->BindBufferBase(GL_SHADER_STORAGE_BUFFER, input->binding, 0);
        break;
    }
    }
}

static void gl_renderpass_run(struct ra *ra,
                              const struct ra_renderpass_run_params *params)
{
    struct ra_gl *p = ra->priv;
    GL *gl = p->gl;
    struct ra_renderpass *pass = params->pass;
    struct ra_renderpass_gl *pass_gl = pass->priv;

    gl->UseProgram(pass_gl->program);

    for (int n = 0; n < params->num_values; n++)
        update_uniform(ra, pass, &params->values[n]);
    gl->ActiveTexture(GL_TEXTURE0);

    switch (pass->params.type) {
    case RA_RENDERPASS_TYPE_RASTER: {
        struct ra_tex_gl *target_gl = params->target->priv;
        assert(params->target->params.render_dst);
        gl->BindFramebuffer(GL_FRAMEBUFFER, target_gl->fbo);
        gl->Viewport(params->viewport.x0, params->viewport.y0,
                     mp_rect_w(params->viewport),
                     mp_rect_h(params->viewport));
        gl->Scissor(params->scissors.x0, params->scissors.y0,
                    mp_rect_w(params->scissors),
                    mp_rect_h(params->scissors));
        gl->Enable(GL_SCISSOR_TEST);
        if (pass->params.enable_blend) {
            gl->BlendFuncSeparate(map_blend(pass->params.blend_src_rgb),
                                  map_blend(pass->params.blend_dst_rgb),
                                  map_blend(pass->params.blend_src_alpha),
                                  map_blend(pass->params.blend_dst_alpha));
            gl->Enable(GL_BLEND);
        }
        gl_vao_draw_data(&pass_gl->vao, GL_TRIANGLES, params->vertex_data,
                         params->vertex_count);
        gl->Disable(GL_SCISSOR_TEST);
        gl->Disable(GL_BLEND);
        gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
        break;
    }
    case RA_RENDERPASS_TYPE_COMPUTE: {
        gl->DispatchCompute(params->compute_groups[0],
                            params->compute_groups[1],
                            params->compute_groups[2]);

        gl->MemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
        break;
    }
    default: abort();
    }

    for (int n = 0; n < params->num_values; n++)
        disable_binding(ra, pass, &params->values[n]);
    gl->ActiveTexture(GL_TEXTURE0);

    gl->UseProgram(0);

    pass_gl->first_run = false;
}

static struct ra_fns ra_fns_gl = {
    .destroy                = gl_destroy,
    .tex_create             = gl_tex_create,
    .tex_destroy            = gl_tex_destroy,
    .tex_upload             = gl_tex_upload,
    .create_mapped_buffer   = gl_create_mapped_buffer,
    .destroy_mapped_buffer  = gl_destroy_mapped_buffer,
    .poll_mapped_buffer     = gl_poll_mapped_buffer,
    .clear                  = gl_clear,
    .blit                   = gl_blit,
    .renderpass_create      = gl_renderpass_create,
    .renderpass_destroy     = gl_renderpass_destroy,
    .renderpass_run         = gl_renderpass_run,
};
