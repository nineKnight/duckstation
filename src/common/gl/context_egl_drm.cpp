#include "context_egl_drm.h"
#include "../log.h"
#include "../assert.h"
#include <drm.h>
#include <drm_fourcc.h>
#include <gbm.h>
Log_SetChannel(GL::ContextEGLDRM);

namespace GL {
ContextEGLDRM::ContextEGLDRM(const WindowInfo& wi) : ContextEGL(wi)
{
  StartPresentThread();
}

ContextEGLDRM::~ContextEGLDRM()
{
  StopPresentThread();
  Assert(!m_current_present_buffer);
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

  return (EGLNativeWindowType)((void*)surface);
}

bool ContextEGLDRM::SwapBuffers()
{
  if (!ContextEGL::SwapBuffers())
    return false;

#if 0
  DRMDisplay::Buffer* front_buffer = GetDisplay()->LockFrontBuffer();
  if (!front_buffer)
    return false;

  GetDisplay()->PresentSurface(front_buffer, m_vsync && m_last_front_buffer);

  if (m_last_front_buffer)
    GetDisplay()->ReleaseBuffer(m_last_front_buffer);

  m_last_front_buffer = front_buffer;
#else
  std::unique_lock lock(m_present_mutex);
  m_present_pending.store(true);
  m_present_cv.notify_one();
  if (m_vsync)
    m_present_done_cv.wait(lock, [this]() { return !m_present_pending.load(); });

#endif

  return true;
}

bool ContextEGLDRM::SetSwapInterval(s32 interval)
{
  if (interval < 0 || interval > 1)
    return false;

  std::unique_lock lock(m_present_mutex);
  m_vsync = (interval > 0);
  return true;
}

void ContextEGLDRM::StartPresentThread()
{
  m_present_thread_shutdown.store(false);
  m_present_thread = std::thread(&ContextEGLDRM::PresentThread, this);
}

void ContextEGLDRM::StopPresentThread()
{
  if (!m_present_thread.joinable())
    return;

  {
    std::unique_lock lock(m_present_mutex);
    m_present_thread_shutdown.store(true);
    m_present_cv.notify_one();
  }

  m_present_thread.join();
}

void ContextEGLDRM::PresentThread()
{
  std::unique_lock lock(m_present_mutex);

  while (!m_present_thread_shutdown.load())
  {
    m_present_cv.wait(lock);

    if (!m_present_pending.load())
      continue;

    DRMDisplay::Buffer* next_buffer = GetDisplay()->LockFrontBuffer();
    const bool wait_for_vsync = m_vsync && m_current_present_buffer;

    lock.unlock();
    GetDisplay()->PresentSurface(next_buffer, wait_for_vsync);
    lock.lock();

    if (m_current_present_buffer)
      GetDisplay()->ReleaseBuffer(m_current_present_buffer);

    m_current_present_buffer = next_buffer;
    m_present_pending.store(false);
    m_present_done_cv.notify_one();
  }

  if (m_current_present_buffer)
  {
    GetDisplay()->ReleaseBuffer(m_current_present_buffer);
    m_current_present_buffer = nullptr;
  }
}

} // namespace GL
