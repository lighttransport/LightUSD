// SPDX-License-Identifier: Apache-2.0
// Asset-free bounded Storm staging control; one surfaceless EGL context.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "pxr/imaging/hdSt/resourceRegistry.h"
#include "pxr/imaging/hdSt/stagingBuffer.h"
#include "pxr/imaging/hgi/blitCmds.h"
#include "pxr/imaging/hgiGL/hgi.h"
#include "pxr/base/tf/errorMark.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
PXR_NAMESPACE_USING_DIRECTIVE
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const size_t pending = static_cast<size_t>(std::strtoull(argv[1], nullptr, 10));
  auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (!platform) return 3;
  EGLDisplay display = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
  EGLint major, minor;
  if (!eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API)) return 4;
  EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
  EGLConfig config;
  EGLint count;
  if (!eglChooseConfig(display, attributes, &config, 1, &count) || count != 1) return 5;
  EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT, EGL_NONE};
  EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
  if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) return 6;
  auto get_string = reinterpret_cast<const unsigned char* (*)(unsigned)>(eglGetProcAddress("glGetString"));
  std::printf("renderer=%s\n", get_string(0x1f01));
  int result = 0;
  {
    TfErrorMark errors;
    HgiGL hgi;
    HdStResourceRegistry registry(&hgi);
    HdStStagingBuffer staging(&registry);
    constexpr size_t chunk = 512 * 1024;
    constexpr size_t copies = 600;
    constexpr size_t offset = 64;
    const size_t bytes = copies * chunk;
    std::vector<unsigned char> source(chunk + 17), output(bytes);
    for (size_t i = 0; i < chunk; ++i) source[i + 17] = static_cast<unsigned char>((i * 7) % 251);
    HgiBufferDesc desc;
    desc.debugName = "bounded-staging-control-destination";
    desc.byteSize = bytes + offset;
    desc.usage = HgiBufferUsageStorage;
    HgiBufferHandle destination = hgi.CreateBuffer(desc);
    staging.Resize(pending);
    for (size_t i = 0; i < copies; ++i) {
      HgiBufferCpuToGpuOp op;
      op.cpuSourceBuffer = source.data();
      op.sourceByteOffset = 17;
      op.gpuDestinationBuffer = destination;
      op.destinationByteOffset = offset + i * chunk;
      op.byteSize = chunk;
      staging.StageCopy(op);
    }
    staging.Flush();
    registry.SubmitBlitWork(HgiSubmitWaitTypeWaitUntilCompleted);
    HgiBufferGpuToCpuOp read;
    read.gpuSourceBuffer = destination;
    read.sourceByteOffset = offset;
    read.byteSize = bytes;
    read.cpuDestinationBuffer = output.data();
    auto commands = hgi.CreateBlitCmds();
    commands->CopyBufferGpuToCpu(read);
    hgi.SubmitCmds(commands.get(), HgiSubmitWaitTypeWaitUntilCompleted);
    size_t mismatch = bytes;
    for (size_t i = 0; i < bytes; ++i) if (output[i] != source[17 + i % chunk]) {mismatch = i; break;}
    std::printf("pending=%zu payload=%zu copies=%zu first_mismatch=%zu errors_clean=%d\n", pending, bytes, copies, mismatch, errors.IsClean());
    if (mismatch != bytes || !errors.IsClean()) result = 7;
    // The first 512 uploads fill the bounded 256 MiB staging space. A
    // later overlapping write must win even when it takes the direct path.
    std::vector<unsigned char> replacement(chunk);
    for (size_t i = 0; i < chunk; ++i) replacement[i] = source[17+i] ^ 0xff;
    staging.Resize(pending);
    for (size_t i = 0; i < 512; ++i) {
      HgiBufferCpuToGpuOp op;
      op.cpuSourceBuffer = source.data();
      op.sourceByteOffset = 17;
      op.gpuDestinationBuffer = destination;
      op.destinationByteOffset = offset + i * chunk;
      op.byteSize = chunk;
      staging.StageCopy(op);
    }
    HgiBufferCpuToGpuOp overwrite;
    overwrite.cpuSourceBuffer = replacement.data();
    overwrite.gpuDestinationBuffer = destination;
    overwrite.destinationByteOffset = offset;
    overwrite.byteSize = chunk;
    staging.StageCopy(overwrite);
    staging.Flush();
    registry.SubmitBlitWork(HgiSubmitWaitTypeWaitUntilCompleted);
    read.byteSize = chunk;
    auto verify = hgi.CreateBlitCmds();
    verify->CopyBufferGpuToCpu(read);
    hgi.SubmitCmds(verify.get(), HgiSubmitWaitTypeWaitUntilCompleted);
    bool ordered = true;
    for (size_t i = 0; i < chunk; ++i) if (output[i] != replacement[i]) {ordered = false; break;}
    std::printf("overlapping_write_order=%d\n", ordered);
    if (!ordered || !errors.IsClean()) result = 8;
    hgi.DestroyBuffer(&destination);
  }
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(display, context);
  eglTerminate(display);
  return result;
}
