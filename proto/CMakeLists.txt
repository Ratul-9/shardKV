# gRPC/Protobuf are optional: the core library, tests and benchmarks build without them.
# The sanitizer presets skip this directory because the Homebrew/system gRPC libraries are not
# instrumented, so TSan would report false races inside them.
set(LLDSTORE_HAVE_GRPC OFF PARENT_SCOPE)
if(NOT LLDSTORE_SANITIZER STREQUAL "none")
  message(STATUS "gRPC targets skipped (sanitizer build)")
  return()
endif()

find_package(Protobuf CONFIG QUIET)
find_package(gRPC CONFIG QUIET)
if(NOT (TARGET gRPC::grpc++ AND TARGET gRPC::grpc_cpp_plugin AND TARGET protobuf::libprotobuf
        AND TARGET protobuf::protoc))
  message(STATUS "gRPC/Protobuf not found: server and client targets skipped")
  return()
endif()

set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})
set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/kv.proto)
set(GEN_SRCS ${GEN_DIR}/kv.pb.cc ${GEN_DIR}/kv.grpc.pb.cc)
set(GEN_HDRS ${GEN_DIR}/kv.pb.h ${GEN_DIR}/kv.grpc.pb.h)

add_custom_command(
  OUTPUT ${GEN_SRCS} ${GEN_HDRS}
  COMMAND protobuf::protoc
          --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
          --cpp_out=${GEN_DIR}
          --grpc_out=${GEN_DIR}
          --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
          ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
  COMMENT "Generating C++ from kv.proto")

# Generated code is compiled without our strict warnings (we do not own it).
add_library(lldstore_proto STATIC ${GEN_SRCS})
target_include_directories(lldstore_proto SYSTEM PUBLIC ${GEN_DIR})
target_link_libraries(lldstore_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)

set(LLDSTORE_HAVE_GRPC ON PARENT_SCOPE)
