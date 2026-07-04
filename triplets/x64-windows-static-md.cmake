# x64-windows-static-md.cmake
# Static library linkage + dynamic CRT (/MD).
#
# Why /MD with static libs?
#   Pure /MT (static CRT) causes problems when your binary loads Windows system
#   DLLs that use the MSVC runtime — two separate CRT heaps result in crashes
#   when free() is called on memory allocated in the other heap.
#   /MD + static libraries is the correct Windows idiom: one shared CRT heap,
#   but Boost/OpenSSL symbols are baked into the .exe (no extra DLLs required).

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)    # /MD — share the CRT heap with Windows system DLLs
set(VCPKG_LIBRARY_LINKAGE static) # Link Boost, OpenSSL etc. statically into the .exe
set(VCPKG_BUILD_TYPE release)
