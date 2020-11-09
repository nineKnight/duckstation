#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/system.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

#if defined(USE_DRMKMS)
#include "drm_host_interface.h"
#endif
#if defined(WIN32)
#include "common/windows_headers.h"
#include "win32_host_interface.h"
#include <shellapi.h>
#endif

static std::unique_ptr<NoGUIHostInterface> CreateHostInterface()
{
  std::unique_ptr<NoGUIHostInterface> host_interface;

#if defined(USE_DRMKMS)
  host_interface = DRMHostInterface::Create();
#endif
#if defined(WIN32)
  host_interface = Win32HostInterface::Create();
#endif

  return host_interface;
}

static int Run(std::unique_ptr<NoGUIHostInterface> host_interface, std::unique_ptr<SystemBootParameters> boot_params)
{
  if (!host_interface->Initialize())
  {
    host_interface->Shutdown();
    return EXIT_FAILURE;
  }

  if (boot_params)
  {
    if (!host_interface->BootSystem(*boot_params))
    {
      host_interface->Shutdown();
      host_interface.reset();
      return EXIT_FAILURE;
    }

    boot_params.reset();
    host_interface->Run();
  }
  else
  {
    std::fprintf(stderr, "No file specified.\n");
  }

  host_interface->Shutdown();
  host_interface.reset();

  return EXIT_SUCCESS;
}

int main(int argc, char* argv[])
{
  std::unique_ptr<NoGUIHostInterface> host_interface = CreateHostInterface();
  std::unique_ptr<SystemBootParameters> boot_params;
  if (!host_interface->ParseCommandLineParameters(argc, argv, &boot_params))
    return EXIT_FAILURE;

  return Run(std::move(host_interface), std::move(boot_params));
}

#ifdef WIN32

int wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
  std::unique_ptr<NoGUIHostInterface> host_interface = CreateHostInterface();
  std::unique_ptr<SystemBootParameters> boot_params;

  {
    int argc;
    LPWSTR* argv_wide = CommandLineToArgvW(lpCmdLine, &argc);
    if (argv_wide)
    {
      std::vector<std::string> argc_strings;
      std::vector<char*> argc_pointers;
      argc_strings.reserve(static_cast<u32>(argc) + 1);
      argc_pointers.reserve(static_cast<u32>(argc) + 1);
      argc_strings.push_back(FileSystem::GetProgramPath());
      for (int i = 0; i < argc; i++)
        argc_strings.push_back(StringUtil::WideStringToUTF8String(argv_wide[i]));
      for (int i = 0; i <= argc; i++)
        argc_pointers.push_back(argc_strings[i].data());
      LocalFree(argv_wide);

      if (!host_interface->ParseCommandLineParameters(argc + 1, argc_pointers.data(), &boot_params))
        return EXIT_FAILURE;
    }
  }

  return Run(std::move(host_interface), std::move(boot_params));
}

#endif