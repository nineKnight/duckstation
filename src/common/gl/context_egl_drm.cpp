#include "context_egl_drm.h"
#include "../log.h"
#include <drm.h>
#include <drm_fourcc.h>
#include <gbm.h>
Log_SetChannel(GL::ContextEGLDRM);

namespace GL {
ContextEGLDRM::ContextEGLDRM(const WindowInfo& wi) : ContextEGL(wi) {}
ContextEGLDRM::~ContextEGLDRM()
{
  if (m_last_front_buffer)
    GetDisplay()->ReleaseBuffer(m_last_front_buffer);
}

std::unique_ptr<Context> ContextEGLDRM::Create(const WindowInfo& wi, const Version* versions_to_try,
                                               size_t num_versions_to_try)
{
  std::unique_ptr<ContextEGLDRM> context = std::make_unique<ContextEGLDRM>(wi);
  if (!context->Initialize(versions_to_try, num_versions_to_try))
    return nullptr;

  return context;
}

std::unique_ptr<Context> ContextEGLDRM::CreateSharedContext(const WindowInfo& wi)
{
  std::unique_ptr<ContextEGLDRM> context = std::make_unique<ContextEGLDRM>(wi);
  context->m_display = m_display;

  if (!context->CreateContextAndSurface(m_version, m_context, false))
    return nullptr;

  return context;
}

void ContextEGLDRM::ResizeSurface(u32 new_surface_width, u32 new_surface_height)
{
  ContextEGL::ResizeSurface(new_surface_width, new_surface_height);
}

bool ContextEGLDRM::SetDisplay()
{
  if (!eglGetPlatformDisplayEXT)
  {
    Log_ErrorPrintf("eglGetPlatformDisplayEXT() not loaded");
    return false;
  }

  m_display = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, GetDisplay()->GetDevice(), nullptr);
  if (!m_display)
  {
    Log_ErrorPrintf("eglGetPlatformDisplayEXT() failed");
    return false;
  }

  return true;
}

EGLNativeWindowType ContextEGLDRM::GetNativeWindow(EGLConfig config)
{
  EGLint visual_id;
  eglGetConfigAttrib(m_display, config, EGL_NATIVE_VISUAL_ID, &visual_id);

  struct gbm_surface* surface =
    GetDisplay()->CreateFramebufferSurface(static_cast<u32>(visual_id), GBM_BO_USE_RENDERING);
  if (!surface)
    return nullptr;

  return reinterpret_cast<EGLNativeDisplayType>(surface);
}

bool ContextEGLDRM::SwapBuffers()
{
  if (!ContextEGL::SwapBuffers())
    return false;

  DRMDisplay::Buffer* front_buffer = GetDisplay()->LockFrontBuffer();
  if (!front_buffer)
    return false;

  GetDisplay()->PresentSurface(front_buffer, m_vsync && m_last_front_buffer);

  if (m_last_front_buffer)
    GetDisplay()->ReleaseBuffer(m_last_front_buffer);

  m_last_front_buffer = front_buffer;
  return true;
}

bool ContextEGLDRM::SetSwapInterval(s32 interval)
{
  if (interval < 0 || interval > 1)
    return false;

  m_vsync = (interval > 0);
  return true;
}

} // namespace GL
