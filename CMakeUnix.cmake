## Find Headers
SET(CMAKE_INCLUDE_CURRENT_DIR ON)

## Add FindPackages macros
LIST(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

## Apache and Apr are required for this part
FIND_PACKAGE(APACHE REQUIRED)
FIND_PACKAGE(APR REQUIRED)

## Configuration files
CONFIGURE_FILE(websocket.load.in websocket.load @ONLY)

## Necessary Includes
INCLUDE_DIRECTORIES(${APACHE_INCLUDE_DIR})
INCLUDE_DIRECTORIES(${APR_INCLUDE_DIR})

## Create The mod_websocket.so
ADD_LIBRARY(mod_websocket MODULE mod_websocket.c)
TARGET_LINK_LIBRARIES(mod_websocket ${APACHE_LIBRARY} ${APR_LIBRARY})

SET_TARGET_PROPERTIES(mod_websocket PROPERTIES PREFIX "")
SET_PROPERTY(TARGET mod_websocket PROPERTY C_STANDARD 11)

## Install Targets
INSTALL(TARGETS mod_websocket DESTINATION ${APACHE_MODULE_DIR})
INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/websocket.load DESTINATION ${APACHE_CONF_DIR}/mods-available)

### Build Examples
IF (BUILD_EXAMPLES)
  FILE(GLOB exfiles ${CMAKE_SOURCE_DIR}/examples/*.c)

  # Construct 1 Target per c files
  FOREACH(it ${exfiles})
    # Get module name
    GET_FILENAME_COMPONENT(modname ${it} NAME_WE)

    # lib
    ADD_LIBRARY(${modname} MODULE ${it})
    TARGET_LINK_LIBRARIES(${modname} ${APACHE_LIBRARY} ${APR_LIBRARY})

    # properties
    SET_TARGET_PROPERTIES(${modname} PROPERTIES PREFIX "")
    SET_PROPERTY(TARGET ${modname} PROPERTY C_STANDARD 11)

    # Install
    INSTALL(TARGETS ${modname} DESTINATION ${APACHE_MODULE_DIR})
  ENDFOREACH(it)
ENDIF(BUILD_EXAMPLES)