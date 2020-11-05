#pragma once
#include "../drm_display.h"
#include "context_egl.h"
#include <atomic>
#include <thread>
#include <condition_variable>
#include <mutex>

namespace GL {

class ContextEGLDRM final : public ContextEGL
{
public:
  ContextEGLDRM(const WindowInfo& wi);
  ~ContextEGLDRM() override;

  static std::unique_ptr<Context> Create(const WindowInfo& wi, const Version* versions_to_try,
                                         size_t num_versions_to_try);

  std::unique_ptr<Context> CreateSharedContext(const WindowInfo& wi) override;
  void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;

  bool SwapBuffers() override;
  bool SetSwapInterval(s32 interval) override;

protected:
  bool SetDisplay() override;
  EGLNativeWindowType GetNativeWindow(EGLConfig config) override;

private:
  DRMDisplay* GetDisplay() { return static_cast<DRMDisplay*>(m_wi.display_connection); }

  void StartPresentThread();
  void StopPresentThread();
  void PresentThread();

  bool m_vsync = true;

  std::thread m_present_thread;
  std::mutex m_present_mutex;
  std::condition_variable m_present_cv;
  std::atomic_bool m_present_pending{ false };
  std::atomic_bool m_present_thread_shutdown{ false };
  std::condition_variable m_present_done_cv;

  DRMDisplay::Buffer* m_current_present_buffer = nullptr;
};

} // namespace GL
