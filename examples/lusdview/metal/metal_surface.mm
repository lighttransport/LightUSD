// SPDX-License-Identifier: Apache-2.0
// Cocoa window attachment only. All GPU commands use the C++ Metal.hpp API.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace lusdview {
void* AttachMetalSurface(GLFWwindow* window, void* device) {
  NSWindow* native = glfwGetCocoaWindow(window);
  CAMetalLayer* layer = [[CAMetalLayer alloc] init];
  layer.device = (id<MTLDevice>)device;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.framebufferOnly = YES;
  layer.contentsScale = native.backingScaleFactor;
  native.contentView.wantsLayer = YES;
  native.contentView.layer = layer;
  return layer;  // caller owns the alloc reference
}
void DetachMetalSurface(GLFWwindow* window, void* surface) {
  NSWindow* native = glfwGetCocoaWindow(window);
  if (native.contentView.layer == (CAMetalLayer*)surface) native.contentView.layer = nil;
}
}  // namespace lusdview
