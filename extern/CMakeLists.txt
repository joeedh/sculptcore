add_subdirectory(eigen)

if (NOT BUILD_WASM)
	set(GLFW_BUILD_DOCS OFF)
	set(GLFW_BUILD_EXAMPLES OFF)
	set(GLFW_BUILD_TESTS OFF)
	set(GLFW_INSTALL OFF)
	add_subdirectory(glfw)
	add_subdirectory(glew)
endif()

