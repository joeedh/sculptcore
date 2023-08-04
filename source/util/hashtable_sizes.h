#pragma once

#include <cstdint>

namespace sculptcore::util {
static size_t hashsizes[] = {
    7,         11,        17,        23,        29,        37,        47,
    59,        79,        101,       127,       163,       211,       269,
    337,       431,       541,       677,       853,       1069,      1361,
    1709,      2137,      2677,      3347,      4201,      5261,      6577,
    8231,      10289,     12889,     16127,     20161,     25219,     31531,
    39419,     49277,     61603,     77017,     96281,     120371,    150473,
    188107,    235159,    293957,    367453,    459317,    574157,    717697,
    897133,    1121423,   1401791,   1752239,   2190299,   2737937,   3422429,
    4278037,   5347553,   6684443,   8355563,   10444457,  13055587,  16319519,
    20399411,  25499291,  31874149,  39842687,  49803361,  62254207,  77817767,
    97272239,  121590311, 151987889, 189984863, 237481091, 296851369, 371064217,
    463830313, 536870909,
};

inline size_t find_hashsize(size_t size)
{
  int prime = 0;
  while (hashsizes[prime] < size) {
    prime++;
  }

  return prime;
}

inline size_t find_hashsize_prev(size_t size)
{
  int prime = 0;
  while (hashsizes[prime + 1] < size) {
    prime++;
  }

  return prime;
}
} // namespace sculptcore::util
