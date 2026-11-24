# The programs' folder holds exactly the third-party DLLs THIRD_PARTY_NOTICES.md names, so the notices
# cover everything a copy of that folder carries. -DDIR=<the programs' folder>
set(named freenect2.dll glfw3.dll libusb-1.0.dll turbojpeg.dll)
file(GLOB found RELATIVE "${DIR}" "${DIR}/*.dll")
list(TRANSFORM found TOLOWER)
list(SORT found)
if(NOT found STREQUAL named)
    message(FATAL_ERROR "the programs' folder holds [${found}], not exactly the four the notices name [${named}]")
endif()
message(STATUS "the programs' folder holds exactly the four named DLLs")