# Cross-compile for 64-bit Windows with MinGW-w64 (e.g. brew install mingw-w64, or apt install mingw-w64).
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=packaging/windows/mingw-toolchain.cmake -DLL_BUNDLED_SDL=ON
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# static runtime: a single .exe with no MinGW DLLs next to it
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc")
