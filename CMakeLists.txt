# PulseCore Game Integration — open SDK (PolyForm Noncommercial 1.0.0).
#
# Layering:
#   pulse-protocol     protocol v1 + JSON     -> no dependencies      (portable C++)
#   pulse-sdk          client SDK             -> pulse-protocol       (Windows named pipe)
#   pulse-test-client  example                -> pulse-sdk
#
# The proprietary effects engine, pipe server, and DualSense / USB-IP bridge live in the closed
# PulseCore repository and are NOT needed to build an integration.

cmake_minimum_required(VERSION 3.20)
project(pulsecore-sdk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Bridge Protocol v1 (open, portable) ---
add_library(pulse-protocol STATIC protocol/src/protocol.cpp)
target_include_directories(pulse-protocol PUBLIC protocol/include)
target_compile_features(pulse-protocol PUBLIC cxx_std_20)

# --- Client SDK (open; uses a Windows named pipe) ---
add_library(pulse-sdk STATIC sdk/src/integration.cpp)
target_include_directories(pulse-sdk PUBLIC sdk/include)
target_link_libraries(pulse-sdk PUBLIC pulse-protocol)
target_compile_features(pulse-sdk PUBLIC cxx_std_20)

# --- Example: a minimal client that connects and sends a few messages ---
add_executable(pulse-test-client examples/test-client/main.cpp)
target_link_libraries(pulse-test-client PRIVATE pulse-sdk pulse-protocol)
target_compile_features(pulse-test-client PRIVATE cxx_std_20)
