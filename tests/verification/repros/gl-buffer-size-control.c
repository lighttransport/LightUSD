/* SPDX-License-Identifier: Apache-2.0
 * Standalone surfaceless EGL/OpenGL allocation-size boundary control.
 * No USD, scene assets, LightUSD or buffer uploads. One allocation at a time. */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <stdint.h>
#include <stdio.h>
int main(void) {
  PFNEGLGETPLATFORMDISPLAYEXTPROC platform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
  if (!platform) return 2;
  EGLDisplay display = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
  EGLint major, minor;
  if (!eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API)) return 3;
  EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
  EGLConfig config;
  EGLint count;
  if (!eglChooseConfig(display, attributes, &config, 1, &count) || count != 1) return 4;
  EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
  EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
  if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) return 5;
  printf("renderer=%s\nversion=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
  GLint64 maximum;
  glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maximum);
  printf("max_shader_storage_block_size=%lld\n", (long long)maximum);
  const uint64_t sizes[] = {UINT64_C(1073741824), UINT64_C(2147483647), UINT64_C(2147483648), UINT64_C(4294967295), UINT64_C(4294967296), UINT64_C(4641997912)};
  for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i) {
    GLuint buffer;
    glCreateBuffers(1, &buffer);
    if (glGetError() != GL_NO_ERROR) return 6;
    glNamedBufferData(buffer, (GLsizeiptr)sizes[i], NULL, GL_DYNAMIC_COPY);
    GLenum error = glGetError();
    GLint64 actual = -1;
    glGetNamedBufferParameteri64v(buffer, GL_BUFFER_SIZE, &actual);
    GLenum query_error = glGetError();
    printf("requested=%llu actual=%lld error=0x%x query_error=0x%x\n", (unsigned long long)sizes[i], (long long)actual, error, query_error);
    fflush(stdout);
    glDeleteBuffers(1, &buffer);
    glFinish();
  }
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(display, context);
  eglTerminate(display);
  return 0;
}
