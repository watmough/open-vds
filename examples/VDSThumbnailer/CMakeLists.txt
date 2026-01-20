
# CMakeLists.txt
add_executable(vds-thumbnailer vds-thumbnailer.cpp)

# link to open-vds
target_link_libraries(vds-thumbnailer openvds::openvds)
set_target_properties(vds-thumbnailer PROPERTIES FOLDER examples)

# flags / install
setWarningFlagsForTarget(vds-thumbnailer)
copyDllForTarget(vds-thumbnailer)
