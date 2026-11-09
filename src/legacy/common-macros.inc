#pragma once

#include <iostream>
#include <sstream>
#include <string>

// Global flag to control DCOUT output. Set via LIGHTUSD_ENABLE_DCOUT environment variable.
namespace lightusd {
extern bool g_enable_dcout_output;

// Default limit for recursive/iterative traversals to prevent infinite loops.
// Used as max recursion depth or max iteration count in tree/graph traversals.
constexpr size_t kMaxDefaultTraversalLimit = 1024 * 1024;
}

#if !defined(LIGHTUSD_PRODUCTION_BUILD) && !defined(LIGHTUSD_FUZZER_BUILD)
#if defined(LIGHTUSD_DEBUG_PRINT)
#define LIGHTUSD_LOCAL_DEBUG_PRINT
#endif
#endif


#if defined(LIGHTUSD_PRODUCTION_BUILD)
// Do not include full filepath for privacy.

#define PUSH_ERROR_AND_RETURN(s)                         \
  do {                                                   \
    std::ostringstream ss_e;                             \
    ss_e << __func__ << "():" << __LINE__ << " ";       \
    ss_e << s;                                           \
    PushError(ss_e.str());                               \
    return false;                                        \
  } while (0)

#define PUSH_ERROR_AND_RETURN_TAG(tag, s)                                    \
  do {                                                                       \
    std::ostringstream ss_e;                                                 \
    ss_e << tag << ":" << __func__ << "():" << __LINE__ << " ";              \
    ss_e << s;                                                               \
    PushError(ss_e.str());                                                   \
    return false;                                                            \
  } while (0)

#define PUSH_ERROR(s)                                    \
  do {                                                   \
    std::ostringstream ss_e;                             \
    ss_e << __func__ << "():" << __LINE__ << " ";       \
    ss_e << s;                                           \
    PushError(ss_e.str());                               \
  } while (0)

#define PUSH_WARN(s)                                     \
  do {                                                   \
    std::ostringstream ss_w;                             \
    ss_w << __func__ << "():" << __LINE__ << " ";       \
    ss_w << s;                                           \
    PushWarn(ss_w.str());                                \
  } while (0)

#else  // LIGHTUSD_PRODUCTION_BUILD

#define PUSH_ERROR_AND_RETURN(s)                                          \
  do {                                                                    \
    std::ostringstream ss_e;                                              \
    ss_e << __FILE__ << ":" << __func__ << "():" << __LINE__              \
         << " ";                                                          \
    ss_e << s;                                                            \
    PushError(ss_e.str());                                                \
    return false;                                                         \
  } while (0)

#define PUSH_ERROR_AND_RETURN_TAG(tag, s)                   \
  do {                                                      \
    std::ostringstream ss_e;                                \
    ss_e << __FILE__ << tag << ":" << __func__             \
         << "():" << __LINE__ << " ";                       \
    ss_e << s;                                              \
    PushError(ss_e.str());                                  \
    return false;                                           \
  } while (0)

#define PUSH_ERROR(s)                                                     \
  do {                                                                    \
    std::ostringstream ss_e;                                              \
    ss_e << __FILE__ << ":" << __func__ << "():" << __LINE__              \
         << " ";                                                          \
    ss_e << s;                                                            \
    PushError(ss_e.str());                                                \
  } while (0)

#define PUSH_WARN(s)                                                     \
  do {                                                                   \
    std::ostringstream ss_w;                                             \
    ss_w << __FILE__ << ":" << __func__ << "():" << __LINE__              \
         << " ";                                                         \
    ss_w << s;                                                           \
    PushWarn(ss_w.str());                                                \
  } while (0)

#endif  // LIGHTUSD_PRODUCTION_BUILD

#if defined(LIGHTUSD_LOCAL_DEBUG_PRINT)
// DCOUT macro - accepts iostream expressions with << operator
// Example usage: DCOUT("ptr = " << std::hex << ptr)
#define DCOUT(x)                                               \
  do {                                                         \
    if (lightusd::g_enable_dcout_output) {                     \
      std::ostringstream dcout_ss;                             \
      dcout_ss << x;                                           \
      std::cout << __FILE__ << ":" << __func__ << ":"          \
                << std::to_string(__LINE__) << " "             \
                << dcout_ss.str() << "\n";                     \
    }                                                          \
  } while (false)
#else
#define DCOUT(x)
#endif


// Simple auto-free class
// Use this class when saving stack size is required(e.g. recursive function call).
// T must have default constructor
template<typename T>
class AutoFree {
 private:
  T *_v{nullptr};

 public:

  AutoFree() : _v(new T()) {
  }

  ~AutoFree() {
    delete _v;
  }

  T &value() { return *_v; }
};
