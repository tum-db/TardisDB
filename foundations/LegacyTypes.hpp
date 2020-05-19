#ifndef H_Types
#define H_Types
//---------------------------------------------------------------------------
#include <cstdint>
#include <cstring>
#include <ostream>
#include <cassert>

#include "codegen/CodeGen.hpp"
//---------------------------------------------------------------------------
// HyPer
// (c) Thomas Neumann 2010
//---------------------------------------------------------------------------
int32_t castStringToIntegerValue(const char* str,size_t strLen);
uint64_t castStringToUnsignedLongIntegerValue(const char* str,size_t strLen);
cg_i32_t genCastStringToIntegerValueCall(cg_ptr8_t str, cg_size_t strLen);
cg_u64_t genCastStringToUnsignedLongIntegerValueCall(cg_ptr8_t str, cg_size_t strLen);
//---------------------------------------------------------------------------
static constexpr uint64_t numericShifts[19]={1ull,10ull,100ull,1000ull,10000ull,100000ull,1000000ull,10000000ull,100000000ull,1000000000ull,10000000000ull,100000000000ull,1000000000000ull,10000000000000ull,100000000000000ull,1000000000000000ull,10000000000000000ull,100000000000000000ull,1000000000000000000ull};
int64_t castStringToNumericValue(const char* str, size_t strLen, unsigned len, unsigned precision);
cg_i64_t genCastStringToNumericValueCall(cg_ptr8_t str, cg_size_t strLen, cg_unsigned_t len, cg_unsigned_t precision);
//---------------------------------------------------------------------------
uint32_t castStringToDateValue(const char* str,uint32_t strLen);
cg_u32_t genCastStringToDateValueCall(cg_ptr8_t str, cg_size_t strLen);
//---------------------------------------------------------------------------
uint64_t castStringToTimestampValue(const char* str,uint32_t strLen);
cg_u64_t genCastStringToTimestampValueCall(cg_ptr8_t str, cg_size_t strLen);
//---------------------------------------------------------------------------
void printNumeric(unsigned len, unsigned precision, int64_t raw);
//---------------------------------------------------------------------------
void printDate(int32_t raw);
//---------------------------------------------------------------------------
void printTimestamp(uint64_t raw);
//---------------------------------------------------------------------------
#endif
