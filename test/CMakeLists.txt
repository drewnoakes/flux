cmake_minimum_required(VERSION 2.8.4)

set(TEST_SOURCES
    unittests.cc

    ../lib/camshaft/src/observable.cc
    ../lib/camshaft/test/any_test.cc
    ../lib/camshaft/test/uuid_test.cc
    ../lib/camshaft/src/demangle.cc
    ../lib/camshaft/src/uuid.cc

    flux_test.cc
    rapidjson_test.cc
    regex_test.cc

    google-test/src/gtest-all.cc
#    $<TARGET_OBJECTS:fritz_objects>
)

#
## Target for running tests. Custom targets are always run
#
add_custom_target(runtests
    ALL ./unittests
    DEPENDS unittests)

#
## Include directories
#
include_directories(SYSTEM google-test/include)
include_directories(google-test)

#
## Link directories
#
# TODO build GTest and link to it so that we don't recompile on every test run
#link_directories(${GOOGLETESTS_BINARY_DIR})
#add_dependencies(runtests googletests)
#set_target_properties(unittests PROPERTIES COMPILE_FLAGS)


#
## Target
#
add_executable(unittests
  ${TEST_SOURCES}
)

#
## Link executable to libraries
#
target_link_libraries(unittests pthread uuid)
