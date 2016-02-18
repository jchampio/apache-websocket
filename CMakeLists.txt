#
# Library Configuration
#

CMAKE_MINIMUM_REQUIRED(VERSION 2.8)
PROJECT(mod_websocket C)

#
# Options
#

OPTION(BUILD_EXAMPLES "Build the example plugins"         OFF)
OPTION(INSTALL_PDB    "Install .pdb files (if generated)" OFF)


IF(WIN32)
  ## Build For Window
  INCLUDE(CMakeWin32.cmake)
ELSE(WIN32)
  ## Build on Unix plateforms using FindApache.cmake and FindAPR.cmake
  INCLUDE(CMakeUnix.cmake)
ENDIF(WIN32)