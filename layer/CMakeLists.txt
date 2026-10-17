cmake_minimum_required(VERSION 3.22)
project(MultiExynoosLayer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MX_VERSION "0.0.1" CACHE STRING "MultiExynoos package version")

include(FetchContent)
FetchContent_Declare(Vulkan-Headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.3.296)
FetchContent_MakeAvailable(Vulkan-Headers)

add_library(VkLayer_MultiExynoos SHARED src/layer.cpp)
target_link_libraries(VkLayer_MultiExynoos PRIVATE Vulkan::Headers)
target_compile_definitions(VkLayer_MultiExynoos
    PRIVATE MX_VERSION_STR="${MX_VERSION}")
target_compile_options(VkLayer_MultiExynoos PRIVATE -Wall -Wextra -Werror)

if(ANDROID)
    # Statically link the C++ runtime so the prebuilt .so does not require
    # libc++_shared.so in the app's native-lib directory.
    target_link_options(VkLayer_MultiExynoos PRIVATE -static-libstdc++)
    target_link_libraries(VkLayer_MultiExynoos PRIVATE log)
endif()
