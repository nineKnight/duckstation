#include "drm_host_interface.h"

DRMHostInterface::DRMHostInterface() = default;

DRMHostInterface::~DRMHostInterface() = default;

std::unique_ptr<NoGUIHostInterface> DRMHostInterface::Create()
{
  return std::make_unique<DRMHostInterface>();
}

bool DRMHostInterface::CreatePlatformWindow()
{
  Assert(!m_drm_display);
  m_drm_display = std::make_unique<DRMDisplay>();
  if (!m_drm_display->Initialize())
  {
    m_drm_display.reset();
    return false;
  }

  return true;
}

void DRMHostInterface::DestroyPlatformWindow()
{
  m_drm_display.reset();
}

std::optional<WindowInfo> DRMHostInterface::GetPlatformWindowInfo()
{
  WindowInfo wi;
  wi.type = WindowInfo::Type::DRM;
  wi.display_connection = m_drm_display.get();
  wi.surface_width = m_drm_display->GetWidth();
  wi.surface_height = m_drm_display->GetHeight();
  wi.surface_format = WindowInfo::SurfaceFormat::Auto;
  return wi;
}
