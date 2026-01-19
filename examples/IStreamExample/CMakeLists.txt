if(WIN32)
  add_executable(istream-example main.cpp)
  target_link_libraries(istream-example openvds::openvds)
  set_target_properties(istream-example PROPERTIES
    FOLDER examples
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
  )

  setWarningFlagsForTarget(istream-example)
  copyDllForTarget(istream-example)
else()
  message(STATUS "IStreamExample is only available on Windows")
endif()
