add_executable(duckstation-nogui
  drm_host_interface.cpp
  drm_host_interface.h
  nogui_host_interface.cpp
  nogui_host_interface.h
  main.cpp
)

if(USE_DRMKMS)
  target_compile_definitions(duckstation-nogui PRIVATE "-DUSE_DRMKMS=1")
endif()

if(LINUX)
  find_package(LIBEVDEV REQUIRED)
  target_compile_definitions(duckstation-nogui PRIVATE "-DUSE_LIBEVDEV=1")
  target_include_directories(duckstation-nogui PRIVATE ${LIBEVDEV_INCLUDE_DIRS})
  target_link_libraries(duckstation-nogui PRIVATE ${LIBEVDEV_LIBRARIES})
endif()

target_link_libraries(duckstation-nogui PRIVATE core common imgui glad frontend-common scmversion simpleini)
