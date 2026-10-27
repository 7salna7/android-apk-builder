cmake_minimum_required(VERSION 3.18.1)
project("mididawpro_core")

include(FetchContent)

# Automātiski lejupielādē un sagatavo Google Oboe dzinēju
FetchContent_Declare(
    oboe
    GIT_REPOSITORY https://github.com
    GIT_TAG        v1.9.3
)
FetchContent_MakeAvailable(oboe)

add_library(mididawpro_core SHARED src/main/cpp/native-lib.cpp)

# Saslēdz kopā Pro-Audio, AMidi un sistēmas logus
target_link_libraries(mididawpro_core
        oboe
        amidi
        log
        android)
