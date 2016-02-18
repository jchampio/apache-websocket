## Shared Library on
set(BUILD_SHARED_LIBS ON)

## Add FindPackages macros
LIST(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

## Apache and Apr are required for this part
find_package(APACHE REQUIRED)
find_package(APR REQUIRED)

## Find the files
file(GLOB srcs ${src}.c)

## Configuration files
configure_file(websocket.load.in websocket.load @ONLY)

## Necessary Includes
include_directories(${CMAKE_SOURCE_DIR})
include_directories(${APACHE_INCLUDE_DIR})
include_directories(${APR_INCLUDE_DIR})

## Create The mod_websocket.so
add_library(mod_websocket MODULE mod_websocket.c)
target_link_libraries(mod_websocket ${APACHE_LIBRARY} ${APR_LIBRARY})

set_target_properties(mod_websocket PROPERTIES PREFIX "")
set_property(TARGET mod_websocket PROPERTY C_STANDARD 11)
set_property(TARGET mod_websocket PROPERTY POSITION_INDEPENDENT_CODE ON)

## Install Targets
install(TARGETS mod_websocket DESTINATION ${APACHE_MODULE_DIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/websocket.load DESTINATION ${APACHE_CONF_DIR}/mods-available)

### Build Examples
if (BUILD_EXAMPLES)
	file(GLOB exfiles ${CMAKE_SOURCE_DIR}/examples/*.c)

	# Construct 1 Target per c files
	foreach(it ${exfiles})
		# Get module name
		get_filename_component(modname ${it} NAME_WE)
		string(REPLACE "mod_" "" confname ${modname})

		# lib
		configure_file(${CMAKE_SOURCE_DIR}/examples/${confname}.load.in ${confname}.load @ONLY)
		add_library(${modname} MODULE ${CMAKE_SOURCE_DIR}/examples/${modname}.c)
		target_link_libraries(${modname}  ${APACHE_LIBRARY} ${APR_LIBRARY})

		# properties
		set_target_properties(${modname} PROPERTIES PREFIX "")
		set_property(TARGET ${modname} PROPERTY C_STANDARD 11)
		set_property(TARGET ${modname} PROPERTY POSITION_INDEPENDENT_CODE ON)

		# install
		install(TARGETS ${modname} DESTINATION ${APACHE_MODULE_DIR})
		install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${confname}.load DESTINATION ${APACHE_CONF_DIR}/mods-available)
	endforeach(it)
endif(BUILD_EXAMPLES)
