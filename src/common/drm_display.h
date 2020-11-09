#pragma once
#include "core/types.h"
#include <array>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

class DRMDisplay
{
public:
  struct Buffer
  {
    struct gbm_bo* bo;
    u32 width;
    u32 height;
    u32 stride;
    u32 format;
    u32 fb_id;
  };

  DRMDisplay(int card = -1);
  ~DRMDisplay();

  bool Initialize();

  u32 GetWidth() const { return m_mode->hdisplay; }
  u32 GetHeight() const { return m_mode->vdisplay; }
  struct gbm_device* GetDevice() const { return m_gbm_device; }
  struct gbm_surface* GetFramebufferSurface() const { return m_fb_surface; }

  struct gbm_surface* CreateFramebufferSurface(u32 fourcc, u32 flags);
  // gbm_surface* CreateSurface(int width, int height);

  Buffer* LockFrontBuffer();
  void ReleaseBuffer(Buffer* buffer);
  void PresentSurface(Buffer* buffer, bool wait_for_vsync);

  void DestroyBuffer(Buffer* buffer);

private:
  enum : u32
  {
    MAX_BUFFERS = 5
  };

  bool TryOpeningCard(int card);

  int m_card_id = 0;
  int m_card_fd = -1;
  u32 m_crtc_id = 0;
  drmModeConnector* m_connector = nullptr;
  drmModeModeInfo* m_mode = nullptr;
  struct gbm_device* m_gbm_device = nullptr;
  struct gbm_surface* m_fb_surface = nullptr;

  std::array<Buffer, MAX_BUFFERS> m_buffers{};
  u32 m_num_buffers = 0;
};
