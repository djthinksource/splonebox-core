find_package(PkgConfig)
if (PKG_CONFIG_FOUND)
  pkg_search_module(SHARED_MSGPACK QUIET
      msgpackc>=${Msgpack_FIND_VERSION}
      msgpack>=${Msgpack_FIND_VERSION})
endif()

find_path(MSGPACK_INCLUDE_DIR msgpack.h
  HINTS ${SHARED_MSGPACK_INCLUDEDIR} ${SHARED_MSGPACK_INCLUDE_DIRS}
  ${LIMIT_SEARCH})
  
list(APPEND MSGPACK_NAMES msgpackc msgpack)

find_library(MSGPACK_LIBRARY NAMES ${MSGPACK_NAMES}
  HINTS ${SHARED_MSGPACK_LIBDIR} ${SHARED_MSGPACK_LIBRARY_DIRS}
  ${LIMIT_SEARCH})

mark_as_advanced(MSGPACK_INCLUDE_DIR MSGPACK_LIBRARY)

set(MSGPACK_LIBRARIES ${MSGPACK_LIBRARY})
set(MSGPACK_INCLUDE_DIRS ${MSGPACK_INCLUDE_DIR})
