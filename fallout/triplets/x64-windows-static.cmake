set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# Enable zlib-ng compat mode so it provides zlib.h API with z_stream
set(ZLIB_COMPAT ON)
