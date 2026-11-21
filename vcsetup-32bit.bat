rmdir /s /q build_win32
mkdir build_win32

rem Fallback for older runner images: cmake -G "Visual Studio 17 2022" -A Win32 -DLIGHTUSD_WITH_OPENSUBDIV=On -DLIGHTUSD_WITH_TINYSUBDIV=Off -Bbuild_win32 -S.

cmake -G "Visual Studio 18 2026" -A Win32 -DLIGHTUSD_WITH_OPENSUBDIV=On -DLIGHTUSD_WITH_TINYSUBDIV=Off -Bbuild_win32 -S.
