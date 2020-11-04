#pragma once
#include "common/drm_display.h"
#include "nogui_host_interface.h"
#include <memory>

class DRMHostInterface final : public NoGUIHostInterface
{
public:
  DRMHostInterface();
  ~DRMHostInterface();

  static std::unique_ptr<NoGUIHostInterface> Create();

protected:
  bool CreatePlatformWindow() override;
  void DestroyPlatformWindow() override;
  std::optional<WindowInfo> GetPlatformWindowInfo() override;

private:
  std::unique_ptr<DRMDisplay> m_drm_display;
};
