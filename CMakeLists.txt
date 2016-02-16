#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# This file is a modified version of the CMakeLists.txt provided by
# Apache httpd.

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
  include(CMakeUnix.cmake)
ENDIF(WIN32)