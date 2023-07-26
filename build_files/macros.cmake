macro(sc_add_library target src lib lib_type)
  add_library(${target} ${lib_type})
  #target_link_libraries(${lib})

  set(SRC_LIST ${src})

  if(${lib_type} MATCHES "INTERFACE")
    set(DEFAULT_SCOPE INTERFACE)
  else()
    set(DEFAULT_SCOPE PRIVATE)
  endif()

  set(SCOPE ${DEFAULT_SCOPE})

  foreach(path IN LISTS SRC_LIST)
    if(path MATCHES "PRIVATE" OR path MATCHES "PUBLIC" OR path MATCHES "INTERFACE")
      set(SCOPE ${path})
    else()
      #message(STATUS "-> ${target} ${SCOPE} ${path}")
      target_sources(${target} ${SCOPE} ${path})

      #Reset back to default scope
      set(SCOPE ${DEFAULT_SCOPE})
    endif()
  endforeach()
endmacro()
