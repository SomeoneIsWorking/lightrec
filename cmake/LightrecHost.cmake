# SPDX-License-Identifier: LGPL-2.1-or-later

function(lightrec_classify_host system_name system_processor output_class output_reason)
	string(TOLOWER "${system_name}" system)
	string(TOLOWER "${system_processor}" processor)

	if (system STREQUAL "linux" AND processor MATCHES "^(x86_64|amd64)$")
		set(class "proven")
		set(reason "Linux x86_64 is covered by the executable runtime contract test")
	elseif (system STREQUAL "darwin" AND processor MATCHES "^(aarch64|arm64)$")
		set(class "refused")
		set(reason "macOS arm64 requires a GNU Lightning MAP_JIT arena and pthread JIT write-protection integration")
	elseif (system STREQUAL "android" AND processor MATCHES "^(aarch64|arm64|arm64-v8a)$")
		set(class "refused")
		set(reason "Android arm64-v8a requires a GNU Lightning backend that reserves platform register x18")
	else()
		set(class "refused")
		set(reason "${system_name}/${system_processor} has no maintained runtime-JIT proof")
	endif()

	set(${output_class} "${class}" PARENT_SCOPE)
	set(${output_reason} "${reason}" PARENT_SCOPE)
endfunction()

function(lightrec_require_proven_host)
	if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
		message(FATAL_ERROR "Lightrec host refused: execution counters require a 64-bit host")
	endif()

	lightrec_classify_host("${CMAKE_SYSTEM_NAME}" "${CMAKE_SYSTEM_PROCESSOR}"
		host_class host_reason)
	if (NOT host_class STREQUAL "proven")
		message(FATAL_ERROR "Lightrec host refused: ${host_reason}")
	endif()
	message(STATUS "Lightrec host capability: ${host_reason}")
endfunction()
