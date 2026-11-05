# NFR-10: "public domain model is not tied to Linux". That is a statement about
# the include graph, so it is checked mechanically rather than by review
# (specs/005-virtual-can-bus-laboratory/research.md R-02).
#
# The Linux UAPI is allowed in exactly two files, both under src/:
# socketcan_fd.hpp and socketcan_transport.cpp.

file(GLOB_RECURSE PUBLIC_HEADERS "${CAN_INCLUDE_DIR}/*.hpp")

if(PUBLIC_HEADERS STREQUAL "")
  message(FATAL_ERROR "no public headers found under ${CAN_INCLUDE_DIR} - the check is vacuous")
endif()

set(OFFENDERS "")
foreach(HEADER IN LISTS PUBLIC_HEADERS)
  file(STRINGS "${HEADER}" HITS REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](linux/|sys/|net/if)")
  if(NOT HITS STREQUAL "")
    list(APPEND OFFENDERS "${HEADER}: ${HITS}")
  endif()
endforeach()

if(NOT OFFENDERS STREQUAL "")
  string(REPLACE ";" "\n  " PRETTY "${OFFENDERS}")
  message(FATAL_ERROR
    "a Linux header leaked into the public surface of tcu_can (NFR-10):\n  ${PRETTY}")
endif()

list(LENGTH PUBLIC_HEADERS COUNT)
message(STATUS "${COUNT} public headers are OS-neutral")
