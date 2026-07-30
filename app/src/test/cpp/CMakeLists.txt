cmake_minimum_required(VERSION 3.16)
project(melonds_phase0_native_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../../main/cpp/enet enet)

add_executable(
        phase0_protocol_codec_test
        ../../main/cpp/multiplayer/Phase0ProtocolCodec.cpp
        Phase0ProtocolCodecTest.cpp
)
target_include_directories(phase0_protocol_codec_test PRIVATE ../../main/cpp)

add_executable(
        phase0_enet_worker_test
        ../../main/cpp/multiplayer/Phase0ProtocolCodec.cpp
        ../../main/cpp/multiplayer/Phase0EnetWorker.cpp
        ../../main/cpp/multiplayer/Phase0TraceRecorder.cpp
        Phase0EnetWorkerTest.cpp
)
target_include_directories(
        phase0_enet_worker_test
        PRIVATE
        ../../main/cpp
        ../../main/cpp/enet/include
)
target_link_libraries(phase0_enet_worker_test PRIVATE enet)

add_executable(
        phase0_core_router_test
        ../../main/cpp/multiplayer/Phase0ProtocolCodec.cpp
        ../../main/cpp/multiplayer/Phase0LanCoreAdapter.cpp
        ../../main/cpp/multiplayer/Phase0SessionRouter.cpp
        ../../main/cpp/multiplayer/Phase0TraceRecorder.cpp
        Phase0CoreRouterTest.cpp
)
target_include_directories(
        phase0_core_router_test
        PRIVATE
        ../../main/cpp
        ../../../../melonDS-android-lib/src
        ../../../../melonDS-android-lib/src/net
)

enable_testing()
add_test(NAME phase0_protocol_codec_test COMMAND phase0_protocol_codec_test)
add_test(NAME phase0_enet_worker_test COMMAND phase0_enet_worker_test)
add_test(NAME phase0_core_router_test COMMAND phase0_core_router_test)
