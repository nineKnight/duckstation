#pragma once
#include "common/window_info.h"
#include "core/host_display.h"
#include "core/host_interface.h"
#include "frontend-common/common_host_interface.h"
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

class AudioStream;

class INISettingsInterface;

class NoGUIHostInterface : public CommonHostInterface
{
public:
  NoGUIHostInterface();
  ~NoGUIHostInterface();

  const char* GetFrontendName() const override;

  bool Initialize() override;
  void Shutdown() override;

  std::string GetStringSettingValue(const char* section, const char* key, const char* default_value = "") override;
  bool GetBoolSettingValue(const char* section, const char* key, bool default_value = false) override;
  int GetIntSettingValue(const char* section, const char* key, int default_value = 0) override;
  float GetFloatSettingValue(const char* section, const char* key, float default_value = 0.0f) override;

  void Run();

protected:
  void LoadSettings() override;
  void SetDefaultSettings(SettingsInterface &si) override;

  bool AcquireHostDisplay() override;
  void ReleaseHostDisplay() override;

  void OnRunningGameChanged() override;
  void OnSystemPerformanceCountersUpdated() override;

  void RequestExit() override;
  virtual void PollAndUpdate() override;

  virtual std::optional<HostKeyCode> GetHostKeyCode(const std::string_view key_code) const override;
  void UpdateInputMap() override;

  virtual bool CreatePlatformWindow() = 0;
  virtual void DestroyPlatformWindow() = 0;
  virtual std::optional<WindowInfo> GetPlatformWindowInfo() = 0;

  void CreateImGuiContext();

  bool IsFullscreen() const override;
  bool SetFullscreen(bool enabled) override;

  bool CreateDisplay();
  void DestroyDisplay();

  std::unique_ptr<INISettingsInterface> m_settings_interface;

  bool m_quit_request = false;
};
