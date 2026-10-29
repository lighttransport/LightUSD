# Embed a text file as a C string literal: -DIN=file -DOUT=file.inc
# Used for the vendored tinyvdb device sampler (tinyvdb_nanovdb_device.h),
# whose text is compiled by NVRTC/hiprtc as part of the GPU compute module's
# device source; the header itself stays a normal header so the CPU reference
# (and tinyvdb's own test) compiles the same code.
file(READ "${IN}" _text)
string(REPLACE "\\" "\\\\" _text "${_text}")
string(REPLACE "\"" "\\\"" _text "${_text}")
string(REPLACE "\n" "\\n\"\n\"" _text "${_text}")
file(WRITE "${OUT}" "static const char tvdb_nanovdb_device_src[] =\n\"${_text}\";\n")
