cmake_minimum_required(VERSION 3.10)
project(lightrec LANGUAGES C VERSION 0.11)

include(cmake/LightrecHost.cmake)
lightrec_require_proven_host()

list(APPEND LIGHTREC_SOURCES
	blockcache.c
	constprop.c
	emitter.c
	execution.c
	interpreter.c
	lightrec.c
	memmanager.c
	optimizer.c
	optimizer-delay-slots.c
	regcache.c
)
list(APPEND LIGHTREC_HEADERS
	blockcache.h
	constprop.h
	debug.h
	disassembler.h
	emitter.h
	execution.h
	interpreter.h
	lightning_compat.h
	lightrec-private.h
	lightrec.h
	memmanager.h
	optimizer.h
	regcache.h
)

add_library(lightrec ${LIGHTREC_SOURCES} ${LIGHTREC_HEADERS})
set_target_properties(lightrec PROPERTIES
	VERSION ${PROJECT_VERSION}
	SOVERSION ${PROJECT_VERSION_MAJOR}
	PUBLIC_HEADER lightrec.h
	C_STANDARD 11
	C_STANDARD_REQUIRED ON
	C_EXTENSIONS OFF
)

set(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared libraries")
if (NOT BUILD_SHARED_LIBS)
	target_compile_definitions(lightrec PRIVATE LIGHTREC_STATIC)
endif (NOT BUILD_SHARED_LIBS)

if (NOT LOG_LEVEL)
	set(LOG_LEVEL Info CACHE STRING "Log level" FORCE)
	set_property(CACHE LOG_LEVEL PROPERTY STRINGS NoLog Error Warning Info Debug)
endif()

if (NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
		"Choose the type of build, options are: None(CMAKE_CXX_FLAGS or CMAKE_C_FLAGS used) Debug Release RelWithDebInfo MinSizeRel."
		FORCE)
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS None Debug Release RelWithDebInfo MinSizeRel)
endif()

string(TOUPPER ${LOG_LEVEL} LIGHTREC_LOG_LEVEL)
target_compile_definitions(lightrec PRIVATE LOG_LEVEL=${LIGHTREC_LOG_LEVEL}_L)

if (CMAKE_COMPILER_IS_GNUCC)
	target_compile_options(lightrec PRIVATE -fvisibility=hidden)
endif()

set(HAS_DEFAULT_ELM ${CMAKE_COMPILER_IS_GNUCC})

option(OPT_REMOVE_DIV_BY_ZERO_SEQ "(optimization) Remove div-by-zero check sequence" ON)
option(OPT_REPLACE_MEMSET "(optimization) Detect and replace memset with host variant" ON)
option(OPT_DETECT_IMPOSSIBLE_BRANCHES "(optimization) Detect impossible branches" ON)
option(OPT_HANDLE_LOAD_DELAYS "(optimization) Detect load delays" ON)
option(OPT_TRANSFORM_OPS "(optimization) Transform opcodes" ON)
option(OPT_LOCAL_BRANCHES "(optimization) Detect local branches" ON)
option(OPT_SWITCH_DELAY_SLOTS "(optimization) Switch delay slots" ON)
option(OPT_FLAG_IO "(optimization) Flag I/O opcodes when the target can be detected" ON)
option(OPT_FLAG_MULT_DIV "(optimization) Flag MULT/DIV that only use one of HI/LO" ON)
option(OPT_EARLY_UNLOAD "(optimization) Unload registers early" ON)
option(OPT_PRELOAD_PC "(optimization) Preload PC value into register" ON)
if (OPT_LOCAL_BRANCHES)
	option(OPT_DETECT_IDLE "(optimization) Detect idle loops" ON)
else()
	set(OPT_DETECT_IDLE OFF)
endif()

if (CMAKE_SYSTEM_PROCESSOR MATCHES "SH4|sh4")
	option(OPT_SH4_USE_GBR "(SH4 optimization) Use GBR register for the state pointer" OFF)
endif()

target_include_directories(lightrec PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(lightrec PRIVATE -Wall)
endif()
if (CMAKE_C_COMPILER_ID STREQUAL "Clang")
	target_compile_options(lightrec PRIVATE -Wno-initializer-overrides)
endif()

option(ENABLE_CODE_BUFFER "Enable external code buffer" ON)
if (ENABLE_CODE_BUFFER)
	target_sources(lightrec PRIVATE tlsf/tlsf.c)
	target_include_directories(lightrec PRIVATE tlsf)
endif (ENABLE_CODE_BUFFER)

find_library(LIBLIGHTNING lightning REQUIRED)
find_path(LIBLIGHTNING_INCLUDE_DIR NAMES lightning.h lightning/lightning.h REQUIRED)

target_include_directories(lightrec PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
	${LIBLIGHTNING_INCLUDE_DIR})
target_link_libraries(lightrec PUBLIC ${LIBLIGHTNING})

if (LOG_LEVEL STREQUAL Debug)
	set(ENABLE_DISASSEMBLER ON)
	target_sources(lightrec PRIVATE disassembler.c)
endif()

configure_file(lightrec-config.h.cmakein lightrec-config.h @ONLY)

include(GNUInstallDirs)
install(TARGETS lightrec
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

include(CTest)
if (BUILD_TESTING)
	add_executable(lightrec_runtime_contract_test tests/runtime_contract_test.c)
	target_link_libraries(lightrec_runtime_contract_test PRIVATE lightrec)
	add_test(NAME lightrec_runtime_contract COMMAND lightrec_runtime_contract_test)

	find_package(Python3 REQUIRED COMPONENTS Interpreter)
	add_test(NAME lightrec_source_contract
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tools/check_runtime_contract.py
			--root ${CMAKE_CURRENT_SOURCE_DIR})
	add_test(NAME lightrec_host_policy
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tools/check_host_policy.py
			--root ${CMAKE_CURRENT_SOURCE_DIR}
			--cmake ${CMAKE_COMMAND})

	option(LIGHTREC_MAINTAINER_CHECKS
		"Run source formatting, Clang analyzer, and structure checks" OFF)
	if (LIGHTREC_MAINTAINER_CHECKS)
		find_program(CLANG_FORMAT_EXECUTABLE clang-format)
		find_program(CLANG_FORMAT_DIFF_EXECUTABLE clang-format-diff)
		find_program(CLANG_TIDY_EXECUTABLE clang-tidy)
		if (NOT CLANG_FORMAT_EXECUTABLE OR
		    NOT CLANG_FORMAT_DIFF_EXECUTABLE OR
		    NOT CLANG_TIDY_EXECUTABLE)
			message(FATAL_ERROR
				"LIGHTREC_MAINTAINER_CHECKS requires clang-format, clang-format-diff, and clang-tidy")
		endif()
		add_test(NAME lightrec_source_quality
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/tools/check_source_quality.py
				--root ${CMAKE_CURRENT_SOURCE_DIR}
				--build ${CMAKE_CURRENT_BINARY_DIR}
				--clang-format ${CLANG_FORMAT_EXECUTABLE}
				--clang-format-diff ${CLANG_FORMAT_DIFF_EXECUTABLE}
				--clang-tidy ${CLANG_TIDY_EXECUTABLE})
	endif()
endif()
