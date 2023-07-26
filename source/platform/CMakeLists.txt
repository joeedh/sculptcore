set(LIB
)

set(SRC
  PUBLIC export.h
  win32.cc
)

sc_add_library(platform "${SRC}" "${LIB}" OBJECT)
