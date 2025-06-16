# Install script for directory: E:/Downloads/KVstorageBaseRaft-cpp-main/KVstorageBaseRaft-cpp-main/src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "E:/Downloads/KVstorageBaseRaft-cpp-main/KVstorageBaseRaft-cpp-main/out/install/x64-Debug")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("E:/Downloads/KVstorageBaseRaft-cpp-main/KVstorageBaseRaft-cpp-main/out/build/x64-Debug/src/skipList/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("E:/Downloads/KVstorageBaseRaft-cpp-main/KVstorageBaseRaft-cpp-main/out/build/x64-Debug/src/common/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("E:/Downloads/KVstorageBaseRaft-cpp-main/KVstorageBaseRaft-cpp-main/out/build/x64-Debug/src/rpc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("E:/Downloads/KVstorageBaseRaft-cpp-main/KVstorageBaseRaft-cpp-main/out/build/x64-Debug/src/fiber/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("E:/Downloads/KVstorageBaseRaft-cpp-main/KVstorageBaseRaft-cpp-main/out/build/x64-Debug/src/raftRpcPro/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("E:/Downloads/KVstorageBaseRaft-cpp-main/KVstorageBaseRaft-cpp-main/out/build/x64-Debug/src/raftCore/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("E:/Downloads/KVstorageBaseRaft-cpp-main/KVstorageBaseRaft-cpp-main/out/build/x64-Debug/src/raftClerk/cmake_install.cmake")
endif()

