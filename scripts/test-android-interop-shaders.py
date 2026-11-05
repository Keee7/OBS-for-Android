#!/usr/bin/env python3
"""Compile the Android interop's actual GLSL against a host ANGLE GLES driver.

This tests a host driver only; it does not validate Android SurfaceTexture or
orientation. Run on Windows: python test-android-interop-shaders.py --egl-dir DIR
"""
import argparse
import ast
import ctypes as c
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'obs-studio/libobs-opengl/gl-android-interop.c'


def shader_source(name):
    text = SOURCE.read_text(encoding='utf-8')
    match = re.search(r'static\s+const\s+char\s*\*\s*' + re.escape(name) + r'\s*=\s*(.*?);\s*\n', text, re.S)
    if not match:
        raise RuntimeError(f'cannot find shader {name} in {SOURCE}')
    body = re.sub(r'/\*.*?\*/|//[^\n]*', '', match.group(1), flags=re.S)
    parts = re.findall(r'"(?:\\.|[^"\\])*"', body)
    if not parts:
        raise RuntimeError(f'no string literals for {name}')
    return ''.join(ast.literal_eval(part) for part in parts).encode('utf-8')


def function(dll, name, result, *args):
    f = getattr(dll, name)
    f.restype = result
    f.argtypes = args
    return f


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--egl-dir', required=True, type=Path, help='directory containing libEGL.dll and libGLESv2.dll')
    opts = parser.parse_args()
    egl_dir = opts.egl_dir.resolve()
    dll_dir = os.add_dll_directory(str(egl_dir))
    egl = c.WinDLL(str(egl_dir / 'libEGL.dll'))
    gl = c.WinDLL(str(egl_dir / 'libGLESv2.dll'))
    I, U, P, F = c.c_int, c.c_uint, c.c_void_p, c.c_float
    eglGetDisplay = function(egl, 'eglGetDisplay', P, P)
    eglInitialize = function(egl, 'eglInitialize', U, P, c.POINTER(I), c.POINTER(I))
    eglBindAPI = function(egl, 'eglBindAPI', U, U)
    eglChooseConfig = function(egl, 'eglChooseConfig', U, P, c.POINTER(I), c.POINTER(P), I, c.POINTER(I))
    eglCreatePbufferSurface = function(egl, 'eglCreatePbufferSurface', P, P, P, c.POINTER(I))
    eglCreateContext = function(egl, 'eglCreateContext', P, P, P, P, c.POINTER(I))
    eglMakeCurrent = function(egl, 'eglMakeCurrent', U, P, P, P, P)
    eglDestroySurface = function(egl, 'eglDestroySurface', U, P, P)
    eglDestroyContext = function(egl, 'eglDestroyContext', U, P, P)
    eglTerminate = function(egl, 'eglTerminate', U, P)
    eglGetError = function(egl, 'eglGetError', U)
    def require(ok, message):
        if not ok:
            raise RuntimeError(f'{message}, EGL error 0x{eglGetError():04x}')
    display = eglGetDisplay(None)
    require(display, 'eglGetDisplay failed')
    surface = context = None
    try:
        major, minor = I(), I()
        require(eglInitialize(display, c.byref(major), c.byref(minor)), 'eglInitialize failed')
        require(eglBindAPI(0x30A0), 'eglBindAPI GLES failed')
        # EGL_SURFACE_TYPE=PBUFFER, R/G/B/A=8, EGL_RENDERABLE_TYPE=ES3.
        attributes = (I * 15)(0x3033, 1, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3040, 0x40, 0x3038, 0, 0)
        config, count = P(), I()
        require(eglChooseConfig(display, attributes, c.byref(config), 1, c.byref(count)) and count.value, 'no GLES3 pbuffer config')
        surface = eglCreatePbufferSurface(display, config, (I * 5)(0x3057, 1, 0x3056, 1, 0x3038))
        require(surface, 'eglCreatePbufferSurface failed')
        context = eglCreateContext(display, config, None, (I * 3)(0x3098, 3, 0x3038))
        require(context, 'eglCreateContext GLES3 failed')
        require(eglMakeCurrent(display, surface, surface, context), 'eglMakeCurrent failed')
        get_string = function(gl, 'glGetString', c.c_char_p, U)
        print('GL_VERSION:', get_string(0x1F02).decode())
        print('GL_RENDERER:', get_string(0x1F01).decode())
        extensions = (get_string(0x1F03) or b'').decode().split()
        create_shader = function(gl, 'glCreateShader', U, U)
        shader_source_fn = function(gl, 'glShaderSource', None, U, I, c.POINTER(c.c_char_p), c.POINTER(I))
        compile_shader = function(gl, 'glCompileShader', None, U)
        get_shader = function(gl, 'glGetShaderiv', None, U, U, c.POINTER(I))
        shader_log = function(gl, 'glGetShaderInfoLog', None, U, I, c.POINTER(I), c.c_char_p)
        create_program = function(gl, 'glCreateProgram', U)
        attach = function(gl, 'glAttachShader', None, U, U)
        link = function(gl, 'glLinkProgram', None, U)
        get_program = function(gl, 'glGetProgramiv', None, U, U, c.POINTER(I))
        program_log = function(gl, 'glGetProgramInfoLog', None, U, I, c.POINTER(I), c.c_char_p)
        delete_shader = function(gl, 'glDeleteShader', None, U)
        delete_program = function(gl, 'glDeleteProgram', None, U)
        def compile_one(kind, name):
            sh = create_shader(kind)
            raw = c.c_char_p(shader_source(name))
            shader_source_fn(sh, 1, c.byref(raw), None)
            compile_shader(sh)
            ok = I(); get_shader(sh, 0x8B81, c.byref(ok))
            if not ok.value:
                log = c.create_string_buffer(8192); shader_log(sh, len(log), None, log)
                delete_shader(sh)
                raise RuntimeError(f'{name} compile failed: {log.value.decode(errors="replace")}')
            print(name, 'compile: PASS')
            return sh
        vertex = compile_one(0x8B31, 'vertex_source')
        def make_program(name):
            fragment = compile_one(0x8B30, name)
            prog = create_program(); attach(prog, vertex); attach(prog, fragment); link(prog)
            ok = I(); get_program(prog, 0x8B82, c.byref(ok))
            delete_shader(fragment)
            if not ok.value:
                log = c.create_string_buffer(8192); program_log(prog, len(log), None, log)
                delete_program(prog)
                raise RuntimeError(f'{name} link failed: {log.value.decode(errors="replace")}')
            print(name, 'link: PASS')
            return prog
        program = make_program('encoder_fragment')
        if 'GL_OES_EGL_image_external_essl3' in extensions:
            external = make_program('external_fragment')
        else:
            print('external_fragment: SKIP (GL_OES_EGL_image_external_essl3 unavailable)')
        # Test actual encoder shader: sample a 1x1 linear RGBA8 texture into a
        # default linear pbuffer and compare its sRGB encoding with CPU math.
        gen_textures = function(gl, 'glGenTextures', None, I, c.POINTER(U))
        bind_texture = function(gl, 'glBindTexture', None, U, U)
        tex_parameter = function(gl, 'glTexParameteri', None, U, U, I)
        tex_image = function(gl, 'glTexImage2D', None, U, I, I, I, I, I, U, U, P)
        gen_vao = function(gl, 'glGenVertexArrays', None, I, c.POINTER(U))
        bind_vao = function(gl, 'glBindVertexArray', None, U)
        use_program = function(gl, 'glUseProgram', None, U)
        uniform_location = function(gl, 'glGetUniformLocation', I, U, c.c_char_p)
        uniform_matrix = function(gl, 'glUniformMatrix4fv', None, I, I, c.c_ubyte, c.POINTER(F))
        uniform_int = function(gl, 'glUniform1i', None, I, I)
        viewport = function(gl, 'glViewport', None, I, I, I, I)
        draw_arrays = function(gl, 'glDrawArrays', None, U, I, I)
        read_pixels = function(gl, 'glReadPixels', None, I, I, I, I, U, U, P)
        get_error = function(gl, 'glGetError', U)
        texture, vao = U(), U()
        gen_textures(1, c.byref(texture)); bind_texture(0x0DE1, texture)
        tex_parameter(0x0DE1, 0x2801, 0x2600); tex_parameter(0x0DE1, 0x2800, 0x2600)
        pixels = (c.c_ubyte * 4)(64, 128, 191, 255)
        tex_image(0x0DE1, 0, 0x8058, 1, 1, 0, 0x1908, 0x1401, pixels)
        gen_vao(1, c.byref(vao)); bind_vao(vao); use_program(program)
        matrix = (F * 16)(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)
        uniform_matrix(uniform_location(program, b'transform'), 1, 0, matrix)
        uniform_int(uniform_location(program, b'image'), 0)
        viewport(0,0,1,1); draw_arrays(4,0,3)
        actual = (c.c_ubyte * 4)(); read_pixels(0,0,1,1,0x1908,0x1401,actual)
        error = get_error()
        if error: raise RuntimeError(f'pixel test GL error 0x{error:04x}')
        def encode(v):
            v /= 255
            return round(255 * (12.92*v if v < 0.0031308 else 1.055*v**(1/2.4)-0.055))
        expected = [encode(x) for x in pixels[:3]] + [255]
        if any(abs(a-b)>2 for a,b in zip(actual, expected)):
            raise RuntimeError(f'pixel conversion mismatch actual={list(actual)} expected={expected}')
        print('encoder linear-to-sRGB pixel: PASS', list(actual), 'expected', expected, '(+/-2)')
        # Exercise external_fragment with an EGLImage backed by a premultiplied
        # color. This checks GLSL + sRGB storage without claiming it models a
        # platform WebView SurfaceTexture or producer matrix.
        query = function(egl, 'eglQueryString', c.c_char_p, P, I)
        egl_ext = (query(display, 0x3055) or b'').decode().split()
        have_external = 'GL_OES_EGL_image_external_essl3' in extensions
        have_image = 'EGL_KHR_image_base' in egl_ext and 'EGL_KHR_gl_texture_2D_image' in egl_ext
        if have_external and have_image:
            get_proc = function(egl, 'eglGetProcAddress', P, c.c_char_p)
            def proc(name, result, *types):
                address = get_proc(name.encode())
                if not address: raise RuntimeError(f'{name} unavailable despite advertised extension')
                return c.WINFUNCTYPE(result, *types)(address)
            create_image = proc('eglCreateImageKHR', P, P, P, U, P, c.POINTER(I))
            destroy_image = proc('eglDestroyImageKHR', U, P, P)
            image_target = proc('glEGLImageTargetTexture2DOES', None, U, P)
            framebuffer_gen = function(gl, 'glGenFramebuffers', None, I, c.POINTER(U))
            framebuffer_bind = function(gl, 'glBindFramebuffer', None, U, U)
            framebuffer_tex = function(gl, 'glFramebufferTexture2D', None, U, U, U, U, I)
            framebuffer_status = function(gl, 'glCheckFramebufferStatus', U, U)
            gl_enable = function(gl, 'glEnable', None, U)
            source_tex, external_tex, target_tex, fbo = U(), U(), U(), U()
            gen_textures(1, c.byref(source_tex)); bind_texture(0x0DE1, source_tex)
            premult = (c.c_ubyte * 4)(64,32,0,128)
            tex_parameter(0x0DE1, 0x2801, 0x2600); tex_parameter(0x0DE1, 0x2800, 0x2600)
            tex_image(0x0DE1, 0, 0x8058, 1, 1, 0, 0x1908, 0x1401, premult)
            img = create_image(display, context, 0x30B1, P(source_tex.value), (I * 3)(0x30D2, 1, 0x3038))
            require(img, 'eglCreateImageKHR from 2D texture failed')
            try:
                gen_textures(1, c.byref(external_tex)); bind_texture(0x8D65, external_tex)
                tex_parameter(0x8D65, 0x2801, 0x2601); tex_parameter(0x8D65, 0x2800, 0x2601)
                tex_parameter(0x8D65, 0x2802, 0x812F); tex_parameter(0x8D65, 0x2803, 0x812F)
                image_target(0x8D65, img)
                gen_textures(1, c.byref(target_tex)); bind_texture(0x0DE1, target_tex)
                tex_parameter(0x0DE1, 0x2801, 0x2600); tex_parameter(0x0DE1, 0x2800, 0x2600)
                tex_image(0x0DE1, 0, 0x8C43, 1, 1, 0, 0x1908, 0x1401, None)
                framebuffer_gen(1, c.byref(fbo)); framebuffer_bind(0x8D40, fbo)
                framebuffer_tex(0x8D40, 0x8CE0, 0x0DE1, target_tex, 0)
                if framebuffer_status(0x8D40) != 0x8CD5: raise RuntimeError('sRGB framebuffer incomplete')
                if 'GL_EXT_sRGB_write_control' in extensions: gl_enable(0x8DB9)
                use_program(external)
                uniform_matrix(uniform_location(external, b'transform'), 1, 0, matrix)
                uniform_int(uniform_location(external, b'image'), 0)
                viewport(0,0,1,1); draw_arrays(4,0,3)
                sample = (c.c_ubyte * 4)(); read_pixels(0,0,1,1,0x1908,0x1401,sample)
                error = get_error()
                if error: raise RuntimeError(f'external pixel test GL error 0x{error:04x}')
                if any(abs(a-b)>2 for a,b in zip(sample, premult)):
                    raise RuntimeError(f'external premult sRGB mismatch actual={list(sample)} expected={list(premult)}')
                print('external premultiplied sRGB pixel: PASS', list(sample), 'expected', list(premult), '(+/-2)')
            finally:
                destroy_image(display, img)
        else:
            print('external premultiplied sRGB pixel: SKIP (required OES/EGLImage extensions unavailable)')
        # Model the OBS default.effect Draw / DrawNonlinearAlpha paths with
        # actual sRGB texture sampling and blending over a colored linear target.
        # The shader algebra below is the GLSL equivalent of default.effect;
        # this does not exercise OBS's effect compiler or Android SurfaceTexture.
        blend_enable = function(gl, 'glEnable', None, U)
        blend_func = function(gl, 'glBlendFuncSeparate', None, U, U, U, U)
        clear_color = function(gl, 'glClearColor', None, F, F, F, F)
        clear = function(gl, 'glClear', None, U)
        fbgen = function(gl, 'glGenFramebuffers', None, I, c.POINTER(U))
        fbbind = function(gl, 'glBindFramebuffer', None, U, U)
        fbtex = function(gl, 'glFramebufferTexture2D', None, U, U, U, U, I)
        fragment_src = b'''#version 300 es
precision highp float; uniform sampler2D image; uniform int straightAlpha;
in vec2 uv; out vec4 color;
vec3 enc(vec3 x){return mix(12.92*x,1.055*pow(x,vec3(1.0/2.4))-0.055,step(vec3(0.0031308),x));}
vec3 dec(vec3 x){return mix(x/12.92,pow((x+0.055)/1.055,vec3(2.4)),step(vec3(0.04045),x));}
void main(){color=texture(image,uv); if(straightAlpha!=0) color.rgb=dec(enc(color.rgb)*color.a);}'''
        frag = create_shader(0x8B30)
        raw = c.c_char_p(fragment_src); shader_source_fn(frag, 1, c.byref(raw), None); compile_shader(frag)
        ok = I(); get_shader(frag, 0x8B81, c.byref(ok))
        if not ok.value: raise RuntimeError('alpha blend fragment compile failed')
        blend_program = create_program(); attach(blend_program, vertex); attach(blend_program, frag); link(blend_program)
        get_program(blend_program, 0x8B82, c.byref(ok))
        if not ok.value: raise RuntimeError('alpha blend program link failed')
        sample_tex, target, blend_fbo = U(), U(), U()
        gen_textures(1, c.byref(sample_tex)); bind_texture(0x0DE1, sample_tex)
        tex_parameter(0x0DE1, 0x2801, 0x2600); tex_parameter(0x0DE1, 0x2800, 0x2600)
        gen_textures(1, c.byref(target)); bind_texture(0x0DE1, target)
        tex_parameter(0x0DE1, 0x2801, 0x2600); tex_parameter(0x0DE1, 0x2800, 0x2600)
        tex_image(0x0DE1, 0, 0x8058, 1, 1, 0, 0x1908, 0x1401, None)
        fbgen(1, c.byref(blend_fbo)); fbbind(0x8D40, blend_fbo); fbtex(0x8D40, 0x8CE0, 0x0DE1, target, 0)
        use_program(blend_program); uniform_matrix(uniform_location(blend_program, b'transform'), 1, 0, matrix)
        uniform_int(uniform_location(blend_program, b'image'), 0)
        blend_enable(0x0BE2); blend_func(1, 0x0303, 1, 0x0303)
        def decode(v):
            return v/12.92 if v <= 0.04045 else ((v+0.055)/1.055)**2.4
        # Target RGB is already premultiplied by target alpha, as OBS expects.
        backgrounds = [((0.0, 0.0, 0.0), 0.0), ((0.025, 0.05, 0.075), 0.25),
                       ((0.1, 0.2, 0.3), 1.0)]
        for background, dst_alpha in backgrounds:
            for a in (0, 128, 255):
                straight_bytes = [128, 64, 32, a]
                premult_bytes = [round(x*a/255) for x in straight_bytes[:3]] + [a]
                for straight_alpha, src in ((1, straight_bytes), (0, premult_bytes)):
                    bind_texture(0x0DE1, sample_tex)
                    pixel = (c.c_ubyte * 4)(*src)
                    tex_image(0x0DE1, 0, 0x8C43, 1, 1, 0, 0x1908, 0x1401, pixel)
                    uniform_int(uniform_location(blend_program, b'straightAlpha'), straight_alpha)
                    clear_color(*background, dst_alpha); clear(0x4000); draw_arrays(4,0,3)
                    actual = (c.c_ubyte * 4)(); read_pixels(0,0,1,1,0x1908,0x1401,actual)
                    premult_rgb = [x*a/255/255 for x in straight_bytes[:3]] if straight_alpha else [x/255 for x in src[:3]]
                    expected = [round(255*(decode(x)+bg*(1-a/255))) for x,bg in zip(premult_rgb, background)]
                    expected.append(round(a + 255 * dst_alpha * (1-a/255)))
                    if any(abs(x-y)>2 for x,y in zip(actual, expected)):
                        raise RuntimeError(f'alpha blend mismatch a={a} dst_alpha={dst_alpha} straight={straight_alpha} actual={list(actual)} expected={expected}')
        print('straight and premultiplied alpha over transparent, translucent and opaque targets: PASS')
        delete_program(blend_program); delete_shader(frag)
        if have_external: delete_program(external)
        delete_program(program); delete_shader(vertex)
    finally:
        eglMakeCurrent(display, None, None, None)
        if context: eglDestroyContext(display, context)
        if surface: eglDestroySurface(display, surface)
        eglTerminate(display)
        dll_dir.close()
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        print('FAIL:', exc, file=sys.stderr)
        sys.exit(1)
