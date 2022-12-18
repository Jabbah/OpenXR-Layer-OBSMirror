project(win-openxr)

set(win-openxr_SOURCES
	win-openxr.cpp)

add_library(win-openxr MODULE
	${win-openxr_SOURCES})
target_link_libraries(win-openxr
	libobs)

install_obs_plugin_with_data(win-openxr data)
