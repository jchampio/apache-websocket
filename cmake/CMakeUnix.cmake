## Find Headers
SET(CMAKE_INCLUDE_CURRENT_DIR ON)

## Add FindPackages macros
LIST(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")

## Apache and Apr are required for this part
FIND_PACKAGE(APACHE REQUIRED)
FIND_PACKAGE(APR REQUIRED)

## Necessary Includes
INCLUDE_DIRECTORIES(${APACHE_INCLUDE_DIR})
INCLUDE_DIRECTORIES(${APR_INCLUDE_DIR})

## Create The mod_websocket.so
ADD_LIBRARY(mod_websocket MODULE mod_websocket.c)
TARGET_LINK_LIBRARIES(mod_websocket ${APR_LIBRARIES})

SET_TARGET_PROPERTIES(mod_websocket
                      PROPERTIES
                        PREFIX     ""
                        C_STANDARD 11)

## Install Targets using apxs
INSTALL(CODE "
  EXECUTE_PROCESS(COMMAND
    ${APACHE_APXS_EXECUTABLE} -i -A -n \"websocket\" mod_websocket.so
  )
")

## Install Headers in APACHE_INCLUDE_DIR
INSTALL(FILES websocket_plugin.h DESTINATION ${APACHE_INCLUDE_DIR})
# INSTALL(FILES validate_utf8.h DESTINATION ${APACHE_INCLUDE_DIR})

### Build Examples
IF (BUILD_EXAMPLES)
  SET(example_targets
        mod_websocket_echo
        mod_websocket_dumb_increment)

  # Construct a target for each example
  FOREACH(modname ${example_targets})
    ADD_LIBRARY(${modname} MODULE examples/${modname}.c)
  ENDFOREACH()

  SET_TARGET_PROPERTIES(${example_targets}
                        PROPERTIES
                          PREFIX     ""
                          C_STANDARD 11)

  # Only the dumb-increment example needs APR.
  TARGET_LINK_LIBRARIES(mod_websocket_dumb_increment ${APR_LIBRARIES})

  # Install using apxs.
  FOREACH(target ${example_targets})
    INSTALL(CODE "
      EXECUTE_PROCESS(COMMAND
        ${APACHE_APXS_EXECUTABLE} -i -n \"dummy\" ${target}.so
      )
    ")
  ENDFOREACH()
ENDIF(BUILD_EXAMPLES)
