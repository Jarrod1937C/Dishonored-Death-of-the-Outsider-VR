cmake_minimum_required(VERSION 3.25)
project(DVR_MockXR CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)

# Headers only from the OpenXR package (this DLL IS a runtime; it must NOT link the loader).
find_package(OpenXR CONFIG REQUIRED)

add_library(DVR_MockXR SHARED mock_runtime.cpp mock_loader_interfaces.h)
target_link_libraries(DVR_MockXR PRIVATE OpenXR::headers d3d11)
set_target_properties(DVR_MockXR PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
