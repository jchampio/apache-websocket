#
# Library Configuration
#

# Default to using APR trunk (libapr-2.lib) if it exists in PREFIX/lib;
# otherwise, default to APR 1.x + APR-util 1.x
IF(EXISTS "${CMAKE_INSTALL_PREFIX}/lib/libapr-2.lib")
  SET(default_apr_libraries "${CMAKE_INSTALL_PREFIX}/lib/libapr-2.lib")
ELSEIF(EXISTS "${CMAKE_INSTALL_PREFIX}/lib/libapr-1.lib")
  SET(default_apr_libraries ${CMAKE_INSTALL_PREFIX}/lib/libapr-1.lib ${CMAKE_INSTALL_PREFIX}/lib/libaprutil-1.lib)
ELSE()
  SET(default_apr_libraries)
ENDIF()

SET(default_httpd_libraries ${CMAKE_INSTALL_PREFIX}/lib/libhttpd.lib)

SET(APR_INCLUDE_DIR    "${CMAKE_INSTALL_PREFIX}/include" CACHE PATH   "Directory with APR[-Util] include files")
SET(APR_LIBRARIES      ${default_apr_libraries}          CACHE STRING "APR libraries to link with")
SET(HTTPD_INCLUDE_DIR  "${CMAKE_INSTALL_PREFIX}/include" CACHE PATH   "Directory with HTTPD include files")
SET(HTTPD_LIBRARIES    ${default_httpd_libraries}        CACHE STRING "HTTPD libraries to link with")
SET(MODULE_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/modules" CACHE PATH   "Directory to install the module into")

#
# Config Checks
#

IF(NOT EXISTS "${APR_INCLUDE_DIR}/apr.h")
  MESSAGE(FATAL_ERROR "APR include directory ${APR_INCLUDE_DIR} is not correct.")
ENDIF()
FOREACH(onelib ${APR_LIBRARIES})
  IF(NOT EXISTS ${onelib})
    MESSAGE(FATAL_ERROR "APR library ${onelib} was not found.")
  ENDIF()
ENDFOREACH()

IF(NOT EXISTS "${HTTPD_INCLUDE_DIR}/httpd.h")
  MESSAGE(FATAL_ERROR "HTTPD include directory ${HTTPD_INCLUDE_DIR} is not correct.")
ENDIF()
FOREACH(onelib ${HTTPD_LIBRARIES})
  IF(NOT EXISTS ${onelib})
    MESSAGE(FATAL_ERROR "HTTPD library ${onelib} was not found.")
  ENDIF()
ENDFOREACH()

IF(NOT EXISTS "${MODULE_INSTALL_DIR}")
  MESSAGE(WARNING "Module installation directory ${MODULE_INSTALL_DIR} does not exist.")
ENDIF()

#
# Module Definition and Compilation
#

INCLUDE_DIRECTORIES(${APR_INCLUDE_DIR} ${HTTPD_INCLUDE_DIR})

ADD_LIBRARY(mod_websocket SHARED mod_websocket.c)
SET_TARGET_PROPERTIES(mod_websocket
                      PROPERTIES
                      SUFFIX ".so")

TARGET_LINK_LIBRARIES(mod_websocket ${APR_LIBRARIES} ${HTTPD_LIBRARIES})

IF(BUILD_EXAMPLES)
  ADD_LIBRARY(mod_websocket_echo           MODULE examples/mod_websocket_echo.c)
  ADD_LIBRARY(mod_websocket_dumb_increment MODULE examples/mod_websocket_dumb_increment.c)
  SET_TARGET_PROPERTIES(mod_websocket_echo mod_websocket_dumb_increment
                        PROPERTIES
                        SUFFIX ".so")

  TARGET_LINK_LIBRARIES(mod_websocket_dumb_increment ${APR_LIBRARIES})
ENDIF()

#
# Installation
#

INSTALL(TARGETS mod_websocket
        RUNTIME DESTINATION ${MODULE_INSTALL_DIR}
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)

INSTALL(FILES websocket_plugin.h DESTINATION include)

IF(BUILD_EXAMPLES)
  INSTALL(TARGETS
            mod_websocket_echo
            mod_websocket_dumb_increment
          LIBRARY DESTINATION ${MODULE_INSTALL_DIR})
ENDIF()

IF(INSTALL_PDB)
  INSTALL(FILES ${PROJECT_BINARY_DIR}/Debug/mod_websocket.pdb
          DESTINATION ${MODULE_INSTALL_DIR}
          CONFIGURATIONS Debug)
  INSTALL(FILES ${PROJECT_BINARY_DIR}/RelWithDebInfo/mod_websocket.pdb
          DESTINATION ${MODULE_INSTALL_DIR}
          CONFIGURATIONS RelWithDebInfo)

  IF(BUILD_EXAMPLES)
    INSTALL(FILES
              ${PROJECT_BINARY_DIR}/Debug/mod_websocket_echo.pdb
              ${PROJECT_BINARY_DIR}/Debug/mod_websocket_dumb_increment.pdb
            DESTINATION ${MODULE_INSTALL_DIR}
            CONFIGURATIONS Debug)
    INSTALL(FILES
              ${PROJECT_BINARY_DIR}/RelWithDebInfo/mod_websocket_echo.pdb
              ${PROJECT_BINARY_DIR}/RelWithDebInfo/mod_websocket_dumb_increment.pdb
            DESTINATION ${MODULE_INSTALL_DIR}
            CONFIGURATIONS RelWithDebInfo)
  ENDIF()
ENDIF()
