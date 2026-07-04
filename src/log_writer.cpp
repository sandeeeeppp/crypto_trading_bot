// log_writer.cpp
//
// All implementation lives in include/log_writer.hpp (inline class definition).
// This translation unit exists so that CMakeLists.txt can list src/log_writer.cpp
// without error, keeping the source list consistent across all V2 phases.
//
// If the LogWriter class ever grows large enough to warrant a separate .cpp
// (e.g., async queue integration in Phase 3), the implementation will be moved
// here and the inline keyword removed from the header.
