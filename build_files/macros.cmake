
macro(sc_add_library target src lib lib_type)
  add_library(${target} ${lib_type})

  set(DEFAULT_SCOPE INTERFACE)
  set(SCOPE ${DEFAULT_SCOPE})

  set(LIB_LIST ${lib})  
  #Dependencies
  foreach(path IN LISTS LIB_LIST)
    if(path MATCHES "PRIVATE" OR path MATCHES "PUBLIC" OR path MATCHES "INTERFACE")
      set(SCOPE ${path})
    else()
      #message(STATUS, "-> ${target} ${SCOPE} ${file_type} ${path}")
      target_link_libraries(${target} ${path})
      target_include_directories(${target} PUBLIC ${path})

      if(MSVC)
        target_compile_options(${target} PUBLIC /std:c++20)
      endif()

      #Reset back to default scope
      set(SCOPE ${DEFAULT_SCOPE})
    endif()
  endforeach()

  #Make it so other libraries include [dir]/header,
  #so we don't have to use XXX_ prefixes to avoid
  #header file name collisions.
  if(${lib_type} MATCHES "INTERFACE")
      target_include_directories(${target} INTERFACE ..)
  else()
      target_include_directories(${target} PUBLIC ..)
  endif()

  set(SRC_LIST ${src})

  if(${lib_type} MATCHES "INTERFACE")
    set(DEFAULT_SCOPE INTERFACE)
  else()
    set(DEFAULT_SCOPE PRIVATE)
  endif()

  #Source files
  set(SCOPE ${DEFAULT_SCOPE})
  foreach(path IN LISTS SRC_LIST)
    if(path MATCHES "PRIVATE" OR path MATCHES "PUBLIC" OR path MATCHES "INTERFACE")
      set(SCOPE ${path})
    else()
      if (path MATCHES ".*\\.h")
        set(file_type "")
      else()
        set(file_type "") 
      endif()

      #message(STATUS, "-> ${target} ${SCOPE} ${file_type} ${path}")
      target_sources(${target} ${SCOPE} ${file_type} ${path})

      #Reset back to default scope
      set(SCOPE ${DEFAULT_SCOPE})
    endif()
  endforeach()
endmacro()


define_property(GLOBAL PROPERTY WASM_SYMBOLS)
macro(wasm_add_symbols symbols)
get_property(existing GLOBAL PROPERTY WASM_SYMBOLS)
set_property(GLOBAL PROPERTY WASM_SYMBOLS "${existing}\n${symbols}")
endmacro()

# sbrush_backend(<name> <validator-exe-or-NONE> [VALIDATOR_ARGS ...])
#
# Per-backend helper that declares one sbrush emitter+validator pair.
# Wave 1 only has the C++ backend, which is linked directly into
# libbrush rather than producing standalone artifacts — so this is
# currently a no-op that records the registration in a global property
# for later use by Wave 3+ backends. The macro signature is the public
# contract documented in brush_compute_dsl.md.
define_property(GLOBAL PROPERTY SBRUSH_BACKENDS)
macro(sbrush_backend name validator)
  get_property(existing GLOBAL PROPERTY SBRUSH_BACKENDS)
  set_property(GLOBAL PROPERTY SBRUSH_BACKENDS "${existing};${name}=${validator}")
endmacro()
