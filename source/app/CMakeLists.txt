set(SRC
	app.cc
)

if(WIN32)
set(GL opengl32.lib)
else()
set(GL gl)
endif()

add_executable(app "${SRC}")
target_link_libraries(app util mesh math platform window glfw ${GL})
target_include_directories(app PUBLIC util mesh math platform window)

