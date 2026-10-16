#pragma once

#if defined(_WIN32)
#  if defined(PEREVIA_OSG_BUILD)
#    define PEREVIA_OSG_API __declspec(dllexport)
#  else
#    define PEREVIA_OSG_API __declspec(dllimport)
#  endif
#else
#  define PEREVIA_OSG_API __attribute__((visibility("default")))
#endif
