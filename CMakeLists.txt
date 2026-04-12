cmake_minimum_required(VERSION 3.16)
project(edh_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SCS_SDK_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/tools/scs_sdk/include")

set(EDH_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EDH_GEN_CPP "${EDH_GEN_DIR}/edh_embedded_resources.cpp")
set(EDH_GEN_H "${EDH_GEN_DIR}/edh_embedded_resources.h")

add_custom_command(
	OUTPUT "${EDH_GEN_CPP}" "${EDH_GEN_H}"
	COMMAND ${CMAKE_COMMAND}
		"-DEDH_RES_DIR=${CMAKE_CURRENT_SOURCE_DIR}/resources"
		"-DEDH_OUT_DIR=${EDH_GEN_DIR}"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/edh_gen_embedded.cmake"
	DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/resources/default.cfg"
		"${CMAKE_CURRENT_SOURCE_DIR}/resources/embed.json"
		"${CMAKE_CURRENT_SOURCE_DIR}/resources/embed_EN.json"
		"${CMAKE_CURRENT_SOURCE_DIR}/resources/embed_NL.json"
		"${CMAKE_CURRENT_SOURCE_DIR}/cmake/edh_gen_embedded.cmake"
	COMMENT "Generating embedded resources"
	VERBATIM
)

add_library(edh_plugin SHARED edh_plugin.cpp "${EDH_GEN_CPP}")
set_source_files_properties("${EDH_GEN_CPP}" PROPERTIES GENERATED TRUE)

target_include_directories(edh_plugin PRIVATE "${SCS_SDK_INCLUDE}" "${EDH_GEN_DIR}")

if(MSVC)
	target_compile_definitions(edh_plugin PRIVATE _CRT_SECURE_NO_WARNINGS)
	target_link_options(edh_plugin PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/edh_plugin.def")
elseif(WIN32)
	target_link_options(edh_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/edh_plugin.def")
endif()

if(WIN32)
	target_link_libraries(edh_plugin PRIVATE winhttp)
endif()

set_target_properties(edh_plugin PROPERTIES PREFIX "" OUTPUT_NAME "edh_plugin")

install(TARGETS edh_plugin RUNTIME DESTINATION .)
