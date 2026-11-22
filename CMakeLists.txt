cmake_minimum_required(VERSION 3.16)
project(BedrockOverLinux VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
find_package(nlohmann_json 3.2 REQUIRED)
find_package(LibArchive REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)

add_executable(bedrock src/main.cpp src/activation.cpp src/import.cpp src/completion.cpp src/core.cpp src/accounts.cpp src/profiles.cpp src/versions.cpp src/tools.cpp src/dependencies.cpp)
target_compile_features(bedrock PRIVATE cxx_std_17)
set_target_properties(bedrock PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(bedrock PRIVATE nlohmann_json::nlohmann_json CLI11::CLI11 pugixml::pugixml)

target_include_directories(bedrock PRIVATE ${LibArchive_INCLUDE_DIRS})
target_link_libraries(bedrock PRIVATE ${LibArchive_LIBRARIES})

install(TARGETS bedrock RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
