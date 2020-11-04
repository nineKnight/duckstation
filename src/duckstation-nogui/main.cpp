#include "common/assert.h"
#include "common/log.h"
#include "core/system.h"
#include "frontend-common/sdl_initializer.h"
#include <cstdio>
#include <cstdlib>

#if defined(USE_DRMKMS)
#include "drm_host_interface.h"
#endif

int main(int argc, char* argv[])
{
  std::unique_ptr<NoGUIHostInterface> host_interface = DRMHostInterface::Create();
  std::unique_ptr<SystemBootParameters> boot_params;
  if (!host_interface->ParseCommandLineParameters(argc, argv, &boot_params))
    return EXIT_FAILURE;

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
