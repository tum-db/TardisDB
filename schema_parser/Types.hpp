#ifndef H_Types_hpp
#define H_Types_hpp

#include <string>
#include <cmath>
#include <cstdint>
#include <string.h>

namespace SchemaParser {

/**
 * Types
 */
namespace Types {
   enum class Tag : unsigned {Integer, Date, Timestamp, Numeric, Char, Varchar};
}


/**
 * Integer
 */
typedef int Integer;


/**
 * Timestamp
 */
typedef uint32_t Date;


/**
 * Timestamp
 */
typedef uint64_t Timestamp;


/**
 * Char
 */
template <unsigned len>
struct Char {
   char data[len];
   void loadString(const std::string& str);
   std::string toString();
};

template <unsigned len>
void Char<len>::loadString(const std::string& str) {
   if (str.size() >= len) {
      memcpy(data, str.c_str(), len);
   } else {
      memset(data, ' ', len);
      memcpy(data, str.c_str(), str.size());
   }
}

template <unsigned len>
std::string Char<len>::toString() {
   return std::string(data, data+len);
}

/**
 * Varchar
 */
template<unsigned len>
using Varchar = Char<len>;

/**
 * Numeric
 */
template <unsigned len1, unsigned len2>
struct Numeric {
   uint64_t data;
   Numeric(uint64_t pre, uint64_t decimal);
};

template <unsigned len1, unsigned len2>
Numeric<len1,len2>::Numeric(uint64_t pre, uint64_t decimal) {
   data = decimal + std::pow(10, len2)*pre;
}

}

#endif
