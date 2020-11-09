#include "drm_host_interface.h"
#include "common/log.h"
#include "common/string_util.h"
#include "evdev_key_names.h"
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
Log_SetChannel(DRMHostInterface);

DRMHostInterface::DRMHostInterface() = default;

DRMHostInterface::~DRMHostInterface()
{
  CloseEVDevFDs();
}

std::unique_ptr<NoGUIHostInterface> DRMHostInterface::Create()
{
  return std::make_unique<DRMHostInterface>();
}

bool DRMHostInterface::Initialize()
{
  if (!NoGUIHostInterface::Initialize())
    return false;

  OpenEVDevFDs();

  signal(SIGTERM, SIGTERMHandler);
  signal(SIGINT, SIGTERMHandler);
  signal(SIGQUIT, SIGTERMHandler);
  return true;
}

void DRMHostInterface::Shutdown()
{
  CloseEVDevFDs();
  NoGUIHostInterface::Shutdown();
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

void DRMHostInterface::PollAndUpdate()
{
  PollEvDevKeyboards();

  NoGUIHostInterface::PollAndUpdate();
}

void DRMHostInterface::OpenEVDevFDs()
{
  for (int i = 0; i < 1000; i++)
  {
    TinyString path;
    path.Format("/dev/input/event%d", i);

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      break;

    struct libevdev* obj;
    if (libevdev_new_from_fd(fd, &obj) != 0)
    {
      Log_ErrorPrintf("libevdev_new_from_fd(%s) failed", path.GetCharArray());
      close(fd);
      continue;
    }

    Log_DevPrintf("Input path: %s", path.GetCharArray());
    Log_DevPrintf("Input device name: \"%s\"", libevdev_get_name(obj));
    Log_DevPrintf("Input device ID: bus %#x vendor %#x product %#x", libevdev_get_id_bustype(obj),
                  libevdev_get_id_vendor(obj), libevdev_get_id_product(obj));
    if (!libevdev_has_event_code(obj, EV_KEY, KEY_SPACE))
    {
      Log_DevPrintf("This device does not look like a keyboard");
      libevdev_free(obj);
      close(fd);
      continue;
    }

    const int grab_res = libevdev_grab(obj, LIBEVDEV_GRAB);
    if (grab_res != 0)
      Log_WarningPrintf("Failed to grab '%s' (%s): %d", libevdev_get_name(obj), path.GetCharArray(), grab_res);

    m_evdev_keyboards.push_back({obj, fd});
  }
}

void DRMHostInterface::CloseEVDevFDs()
{
  for (const EvDevKeyboard& kb : m_evdev_keyboards)
  {
    libevdev_grab(kb.obj, LIBEVDEV_UNGRAB);
    libevdev_free(kb.obj);
    close(kb.fd);
  }
  m_evdev_keyboards.clear();
}

void DRMHostInterface::PollEvDevKeyboards()
{
  for (const EvDevKeyboard& kb : m_evdev_keyboards)
  {
    struct input_event ev;
    while (libevdev_next_event(kb.obj, LIBEVDEV_READ_FLAG_NORMAL, &ev) == 0)
    {
      // auto-repeat
      if (ev.value == 2)
        continue;

      const bool pressed = (ev.value == 1);
      const HostKeyCode code = static_cast<HostKeyCode>(ev.code);
      HandleHostKeyEvent(code, pressed);
    }
  }
}

std::optional<DRMHostInterface::HostKeyCode> DRMHostInterface::GetHostKeyCode(const std::string_view key_code) const
{
  std::optional<int> kc = EvDevKeyNames::GetKeyCodeForName(key_code);
  if (!kc.has_value())
    return std::nullopt;

  return static_cast<HostKeyCode>(kc.value());
}

void DRMHostInterface::SIGTERMHandler(int sig)
{
  Log_InfoPrintf("Recieved SIGTERM");
  static_cast<DRMHostInterface*>(g_host_interface)->m_quit_request = true;
  signal(sig, SIG_DFL);
}
