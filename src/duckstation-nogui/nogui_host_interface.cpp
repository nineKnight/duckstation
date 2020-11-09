#include "nogui_host_interface.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/system.h"
#include "frontend-common/controller_interface.h"
#include "frontend-common/icon.h"
#include "frontend-common/imgui_styles.h"
#include "frontend-common/ini_settings_interface.h"
#include "frontend-common/opengl_host_display.h"
#include "frontend-common/vulkan_host_display.h"
#include "scmversion/scmversion.h"
#include <cinttypes>
#include <cmath>
#include <imgui.h>
#include <imgui_stdlib.h>
Log_SetChannel(NoGUIHostInterface);

#ifdef WIN32
#include "frontend-common/d3d11_host_display.h"
#endif

NoGUIHostInterface::NoGUIHostInterface() = default;

NoGUIHostInterface::~NoGUIHostInterface() = default;

const char* NoGUIHostInterface::GetFrontendName() const
{
  return "DuckStation NoGUI Frontend";
}

ALWAYS_INLINE static TinyString GetWindowTitle()
{
  return TinyString::FromFormat("DuckStation %s (%s)", g_scm_tag_str, g_scm_branch_str);
}

void NoGUIHostInterface::CreateImGuiContext()
{
  const float framebuffer_scale = 1.0f;

  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  // ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  ImGui::GetIO().DisplayFramebufferScale.x = framebuffer_scale;
  ImGui::GetIO().DisplayFramebufferScale.y = framebuffer_scale;
  ImGui::GetStyle().ScaleAllSizes(framebuffer_scale);

  ImGui::StyleColorsDarker();
  ImGui::AddRobotoRegularFont(15.0f * framebuffer_scale);
}

bool NoGUIHostInterface::CreateDisplay()
{
  std::optional<WindowInfo> wi = GetPlatformWindowInfo();
  if (!wi)
  {
    ReportError("Failed to get platform window info");
    return false;
  }

  CreateImGuiContext();

  std::unique_ptr<HostDisplay> display;
  switch (g_settings.gpu_renderer)
  {
    case GPURenderer::HardwareVulkan:
      display = std::make_unique<FrontendCommon::VulkanHostDisplay>();
      break;

    case GPURenderer::HardwareOpenGL:
#ifndef WIN32
    default:
#endif
      display = std::make_unique<FrontendCommon::OpenGLHostDisplay>();
      break;

#ifdef WIN32
    case GPURenderer::HardwareD3D11:
    default:
      display = std::make_unique<FrontendCommon::D3D11HostDisplay>();
      break;
#endif
  }

  Assert(display);
  if (!display->CreateRenderDevice(wi.value(), g_settings.gpu_adapter, g_settings.gpu_use_debug_device) ||
      !display->InitializeRenderDevice(GetShaderCacheBasePath(), g_settings.gpu_use_debug_device))
  {
    ReportError("Failed to create/initialize display render device");
    return false;
  }

  m_display = std::move(display);
  ImGui::NewFrame();
  return true;
}

void NoGUIHostInterface::DestroyDisplay()
{
  if (m_display)
    m_display->DestroyRenderDevice();
  m_display.reset();

  if (ImGui::GetCurrentContext())
    ImGui::DestroyContext();
}

bool NoGUIHostInterface::AcquireHostDisplay()
{
  if (!CreateHostDisplayResources())
    return false;

  return true;
}

void NoGUIHostInterface::ReleaseHostDisplay()
{
  ReleaseHostDisplayResources();

  // restore vsync, since we don't want to burn cycles at the menu
  m_display->SetVSync(true);
}

std::optional<CommonHostInterface::HostKeyCode>
NoGUIHostInterface::GetHostKeyCode(const std::string_view key_code) const
{
  return std::nullopt;
}

void NoGUIHostInterface::UpdateInputMap()
{
  CommonHostInterface::UpdateInputMap(*m_settings_interface.get());
}

void NoGUIHostInterface::OnRunningGameChanged()
{
  CommonHostInterface::OnRunningGameChanged();

  Settings old_settings(std::move(g_settings));
  CommonHostInterface::LoadSettings(*m_settings_interface.get());
  CommonHostInterface::ApplyGameSettings(true);
  CommonHostInterface::FixIncompatibleSettings(true);
  CheckForSettingsChanges(old_settings);
}

void NoGUIHostInterface::OnSystemPerformanceCountersUpdated()
{
  HostInterface::OnSystemPerformanceCountersUpdated();

  Log_InfoPrintf("FPS: %.2f VPS: %.2f Average: %.2fms Worst: %.2fms", System::GetFPS(), System::GetVPS(),
                 System::GetAverageFrameTime(), System::GetWorstFrameTime());
}

void NoGUIHostInterface::RequestExit()
{
  m_quit_request = true;
}

void NoGUIHostInterface::PollAndUpdate()
{
  CommonHostInterface::PollAndUpdate();

  if (m_controller_interface)
    m_controller_interface->PollEvents();
}

bool NoGUIHostInterface::IsFullscreen() const
{
  return true;
}

bool NoGUIHostInterface::SetFullscreen(bool enabled)
{
  return false;
}

bool NoGUIHostInterface::Initialize()
{
  if (!CommonHostInterface::Initialize())
    return false;

  if (!CreatePlatformWindow())
  {
    Log_ErrorPrintf("Failed to create platform window");
    return false;
  }

  if (!CreateDisplay())
  {
    Log_ErrorPrintf("Failed to create host display");
    return false;
  }

  // Change to the user directory so that all default/relative paths in the config are after this.
  if (!FileSystem::SetWorkingDirectory(m_user_directory.c_str()))
    Log_ErrorPrintf("Failed to set working directory to '%s'", m_user_directory.c_str());

  // process events to pick up controllers before updating input map
  UpdateInputMap();

  // we're always in batch mode for now
  m_batch_mode = true;
  return true;
}

void NoGUIHostInterface::Shutdown()
{
  CommonHostInterface::Shutdown();

  DestroyDisplay();
  DestroyPlatformWindow();
}

std::string NoGUIHostInterface::GetStringSettingValue(const char* section, const char* key,
                                                      const char* default_value /*= ""*/)
{
  return m_settings_interface->GetStringValue(section, key, default_value);
}

bool NoGUIHostInterface::GetBoolSettingValue(const char* section, const char* key, bool default_value /* = false */)
{
  return m_settings_interface->GetBoolValue(section, key, default_value);
}

int NoGUIHostInterface::GetIntSettingValue(const char* section, const char* key, int default_value /* = 0 */)
{
  return m_settings_interface->GetIntValue(section, key, default_value);
}

float NoGUIHostInterface::GetFloatSettingValue(const char* section, const char* key, float default_value /* = 0.0f */)
{
  return m_settings_interface->GetFloatValue(section, key, default_value);
}

void NoGUIHostInterface::LoadSettings()
{
  // Settings need to be loaded prior to creating the window for OpenGL bits.
  m_settings_interface = std::make_unique<INISettingsInterface>(GetSettingsFileName());
  if (!CommonHostInterface::CheckSettings(*m_settings_interface.get()))
    m_settings_interface->Save();

  CommonHostInterface::LoadSettings(*m_settings_interface.get());
  CommonHostInterface::FixIncompatibleSettings(false);

  // Some things we definitely don't want.
  g_settings.confim_power_off = false;
}

void NoGUIHostInterface::SetDefaultSettings(SettingsInterface& si)
{
  CommonHostInterface::SetDefaultSettings(si);

  si.SetBoolValue("Main", "ConfirmPowerOff", false);

  si.SetStringValue("Logging", "LogLevel", "Info");
  si.SetBoolValue("Logging", "LogToConsole", true);

  si.SetBoolValue("Display", "ShowOSDMessages", true);
  si.SetBoolValue("Display", "ShowFPS", false);
  si.SetBoolValue("Display", "ShowVPS", false);
  si.SetBoolValue("Display", "ShowSpeed", false);
  si.SetBoolValue("Display", "ShowResolution", false);
}

void NoGUIHostInterface::Run()
{
  while (!m_quit_request)
  {
    PollAndUpdate();

    if (System::IsRunning())
    {
      System::RunFrame();
      UpdateControllerRumble();
      if (m_frame_step_request)
      {
        m_frame_step_request = false;
        PauseSystem(true);
      }
    }

    // rendering
    {
      DrawImGuiWindows();

      m_display->Render();
      ImGui::NewFrame();

      if (System::IsRunning())
      {
        System::UpdatePerformanceCounters();

        if (m_speed_limiter_enabled)
          System::Throttle();
      }
    }
  }

  // Save state on exit so it can be resumed
  if (!System::IsShutdown())
  {
    if (g_settings.save_state_on_exit)
      SaveResumeSaveState();
    DestroySystem();
  }
}
