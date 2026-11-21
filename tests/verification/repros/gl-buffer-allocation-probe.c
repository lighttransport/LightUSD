#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <execinfo.h>
#include <GL/glcorearb.h>
/* Diagnostic interposer for Garch's explicit libGL symbol lookup. No GL error
 * is consumed. Stop at the first allocation whose resulting size is wrong. */
typedef void* (*lookup_fn)(void*, const char*);
typedef void* (*proc_fn)(const char*);
static proc_fn get_proc;
static PFNGLNAMEDBUFFERDATAPROC named_data;
static PFNGLGETNAMEDBUFFERPARAMETERI64VPROC get_size;
static void probe_named_data(GLuint buffer, GLsizeiptr bytes, const void* data, GLenum usage) {
  named_data(buffer, bytes, data, usage);
  GLint64 actual = -1;
  get_size(buffer, GL_BUFFER_SIZE, &actual);
  if (bytes >= 128*1024*1024 || actual != bytes)
    fprintf(stderr,"[GL_BUFFER_PROBE] buffer=%u requested=%lld actual=%lld usage=%u\n",buffer,(long long)bytes,(long long)actual,usage);
  if (bytes > 0 && actual != bytes) {
    PFNGLGETOBJECTLABELPROC get_label=(PFNGLGETOBJECTLABELPROC)get_proc("glGetObjectLabel");
    char label[512]={0};
    GLsizei length=0;
    if (get_label) get_label(GL_BUFFER,buffer,sizeof(label),&length,label);
    fprintf(stderr,"[GL_BUFFER_PROBE] label=%s\n",label);
    void* frames[24];
    int count=backtrace(frames,24);
    backtrace_symbols_fd(frames,count,STDERR_FILENO);
    fflush(stderr);
    _exit(86);
  }
}
static void* probe_proc(const char* name) {
  void* result = get_proc(name);
  if (!strcmp(name,"glNamedBufferData") && result) {
    named_data=(PFNGLNAMEDBUFFERDATAPROC)result;
    get_size=(PFNGLGETNAMEDBUFFERPARAMETERI64VPROC)get_proc("glGetNamedBufferParameteri64v");
    if (get_size) {fprintf(stderr,"[GL_BUFFER_PROBE] intercepted glNamedBufferData\n");return (void*)probe_named_data;}
  }
  return result;
}
void* dlsym(void* handle, const char* name) {
  lookup_fn real=(lookup_fn)dlvsym(RTLD_NEXT,"dlsym","GLIBC_2.2.5");
  void* result=real(handle,name);
  if (!strcmp(name,"glXGetProcAddressARB") && result) {
    get_proc=(proc_fn)result;
    return (void*)probe_proc;
  }
  return result;
}
