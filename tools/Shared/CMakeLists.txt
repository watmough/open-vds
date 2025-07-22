set(TOOLS_SHARED_SOURCES
  HelpConnection.h
  HelpConnection.cpp)

if(WIN32)
  list(APPEND TOOLS_SHARED_SOURCES
    WindowsUnicodeMain.h
    WindowsUnicodeMain.cpp)
endif()

add_library(tools_shared OBJECT ${TOOLS_SHARED_SOURCES})
target_link_libraries(tools_shared PUBLIC help::connection)
setCompilerFlagsForTarget(tools_shared)
