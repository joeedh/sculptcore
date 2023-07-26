# Install script for directory: C:/dev/sculptcore/extern/eigen/unsupported/Eigen

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/dev/sculptcore/extern/eigen_export")
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

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xDevelx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/eigen3/unsupported/Eigen" TYPE FILE FILES
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/AdolcForward"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/AlignedVector3"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/ArpackSupport"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/AutoDiff"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/BVH"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/EulerAngles"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/FFT"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/IterativeSolvers"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/KroneckerProduct"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/LevenbergMarquardt"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/MatrixFunctions"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/MoreVectorization"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/MPRealSupport"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/NonLinearOptimization"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/NumericalDiff"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/OpenGLSupport"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/Polynomials"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/Skyline"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/SparseExtra"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/SpecialFunctions"
    "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/Splines"
    )
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xDevelx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/eigen3/unsupported/Eigen" TYPE DIRECTORY FILES "C:/dev/sculptcore/extern/eigen/unsupported/Eigen/src" FILES_MATCHING REGEX "/[^/]*\\.h$")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("C:/dev/sculptcore/extern/eigen/out/unsupported/Eigen/CXX11/cmake_install.cmake")

endif()

