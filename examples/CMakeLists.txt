project(simple_example)

add_executable(simple_example simple_example.cc)

target_link_libraries(simple_example
        ${ROCKSDB_LIB})