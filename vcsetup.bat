rmdir /s /q build
mkdir build

rem Fallback for older runner images: cmake -G "Visual Studio 17 2022" -A x64 -DLIGHTUSD_WITH_OPENSUBDIV=On -DLIGHTUSD_WITH_TINYSUBDIV=Off -Bbuild -H.

cmake -G "Visual Studio 18 2026" -A x64 -DLIGHTUSD_WITH_OPENSUBDIV=On -DLIGHTUSD_WITH_TINYSUBDIV=Off -Bbuild -H.
