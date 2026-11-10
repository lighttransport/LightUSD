@echo off
rem Bootstrap an MSVC build using the Visual Studio 2026 (v18) toolchain.
rem CMake 4.1.x has no "Visual Studio 18 2026" generator yet, so we enter the
rem VS2026 developer environment (vcvars64) and build with Ninja + MSVC.

rmdir /s /q build_msvc26
mkdir build_msvc26

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DLIGHTUSD_WITH_OPENSUBDIV=On -DLIGHTUSD_WITH_TINYSUBDIV=Off -Bbuild_msvc26 -H.

cmake --build build_msvc26 --parallel
