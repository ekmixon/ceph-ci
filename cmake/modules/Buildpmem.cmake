function(build_pmem)
  include(FindMake)
  find_make("MAKE_EXECUTABLE" "make_cmd")

  if(EXISTS "${PROJECT_SOURCE_DIR}/src/pmdk/Makefile")
    set(source_dir_args
      SOURCE_DIR "${PROJECT_SOURCE_DIR}/src/pmdk")
  else()
    set(source_dir_args
      SOURCE_DIR ${CMAKE_BINARY_DIR}/src/pmdk
      GIT_REPOSITORY https://github.com/ceph/pmdk.git
      GIT_TAG "1.10"
      GIT_SHALLOW TRUE
      GIT_CONFIG advice.detachedHead=false)
  endif()

  # Use debug PMDK libs in debug lib/rbd builds
  if(CMAKE_BUILD_TYPE STREQUAL Debug)
    set(PMDK_LIB_DIR "debug")
  else()
    set(PMDK_LIB_DIR "nondebug")
  endif()

  include(ExternalProject)
  ExternalProject_Add(pmdk_ext
      ${source_dir_args}
      CONFIGURE_COMMAND ""
      BUILD_COMMAND ${make_cmd} CC=${CMAKE_C_COMPILER} EXTRA_CFLAGS=-Wno-error BUILD_EXAMPLES=n BUILD_BENCHMARKS=n DOC=n
      BUILD_IN_SOURCE 1
      BUILD_BYPRODUCTS "<SOURCE_DIR>/src/${PMDK_LIB_DIR}/libpmem.a" "<SOURCE_DIR>/src/${PMDK_LIB_DIR}/libpmemobj.a"
      INSTALL_COMMAND "")
  unset(make_cmd)

  ExternalProject_Get_Property(pmdk_ext source_dir)
  set(PMDK_SRC "${source_dir}/src")
  set(PMDK_INCLUDE "${source_dir}/src/include")
  set(PMDK_LIB "${source_dir}/src/${PMDK_LIB_DIR}")

  # libpmem
  add_library(pmem::pmem STATIC IMPORTED GLOBAL)
  add_dependencies(pmem::pmem pmdk_ext)
  file(MAKE_DIRECTORY ${PMDK_INCLUDE})
  find_package(Threads)
  set_target_properties(pmem::pmem PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${PMDK_INCLUDE}
    IMPORTED_LOCATION "${PMDK_LIB}/libpmem.a"
    INTERFACE_LINK_LIBRARIES "Threads::Threads;ndctl::ndctl")

  # libpmemobj
  add_library(pmem::pmemobj STATIC IMPORTED GLOBAL)
  add_dependencies(pmem::pmemobj pmdk_ext)
  set_target_properties(pmem::pmemobj PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${PMDK_INCLUDE}
    IMPORTED_LOCATION "${PMDK_LIB}/libpmemobj.a"
    INTERFACE_LINK_LIBRARIES "pmem::pmem;${CMAKE_THREAD_LIBS_INIT}")
endfunction()
