#include "foundations/LegacyTypes.hpp"
//---------------------------------------------------------------------------
#include <ctime>
#include <llvm/IR/TypeBuilder.h>
//---------------------------------------------------------------------------
// HyPer
// (c) Thomas Neumann 2010
//---------------------------------------------------------------------------
using namespace std;
//---------------------------------------------------------------------------
int32_t castStringToIntegerValue(const char* str, size_t strLen)
// Cast a string to an integer value
{
   auto iter=str,limit=str+strLen;

   // Trim WS
   while ((iter!=limit)&&((*iter)==' ')) ++iter;
   while ((iter!=limit)&&((*(limit-1))==' ')) --limit;

   // Check for a sign
   bool neg=false;
   if (iter!=limit) {
      if ((*iter)=='-') {
         neg=true;
         ++iter;
      } else if ((*iter)=='+') {
         ++iter;
      }
   }

   // Parse
   if (iter==limit)
      throw "invalid number format: found non-integer characters";

   int64_t result=0;
   unsigned digitsSeen=0;
   for (;iter!=limit;++iter) {
      char c=*iter;
      if ((c>='0')&&(c<='9')) {
         result=(result*10)+(c-'0');
         ++digitsSeen;
      } else if (c=='.') {
         break;
      } else {
         throw "invalid number format: invalid character in integer string";
      }
   }

   if (digitsSeen>10)
      throw "invalid number format: too many characters (32bit integers can at most consist of 10 numeric characters)";

    int32_t r = neg?-result:result;
   return r;
}
//---------------------------------------------------------------------------
cg_i32_t genCastStringToIntegerValueCall(cg_ptr8_t str, cg_size_t strLen)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<int32_t (const char *, size_t), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&castStringToIntegerValue, funcTy, {str, strLen});

    return cg_i32_t( llvm::cast<llvm::Value>(result) );
}
//---------------------------------------------------------------------------
int64_t castStringToNumericValue(const char* str, size_t strLen, unsigned len, unsigned precision)
{
    auto iter=str,limit=str+strLen;

    // Trim WS
    while ((iter!=limit)&&((*iter)==' ')) ++iter;
    while ((iter!=limit)&&((*(limit-1))==' ')) --limit;

    // Check for a sign
    bool neg=false;
    if (iter!=limit) {
        if ((*iter)=='-') {
            neg=true;
            ++iter;
        } else if ((*iter)=='+') {
            ++iter;
        }
    }

    // Parse
    if (iter==limit)
        throw "invalid number format: found non-numeric characters";

    int64_t result=0;
    bool fraction=false;
    unsigned digitsSeen=0;
    unsigned digitsSeenFraction=0;
    for (;iter!=limit;++iter) {
        char c=*iter;
        if ((c>='0')&&(c<='9')) {
            if (fraction) {
                result=(result*10)+(c-'0');
                ++digitsSeenFraction;
            } else {
                ++digitsSeen;
                result=(result*10)+(c-'0');
            }
        } else if (c=='.') {
            if (fraction)
                throw "invalid number format: already in fraction";
            while ((iter!=limit)&&((*(limit-1))=='0')) --limit; // skip trailing 0s
            fraction=true;
        } else {
            throw "invalid number format: invalid character in numeric string";
        }
    }

    if ((digitsSeen>18/*(len-precision)*/)||(digitsSeenFraction>precision))
        throw "invalid number format: loosing precision";

    result*=numericShifts[precision-digitsSeenFraction];
    if (neg) {
        return -result;
    } else {
        return result;
    }
}
//---------------------------------------------------------------------------
cg_i64_t genCastStringToNumericValueCall(cg_ptr8_t str, cg_size_t strLen, cg_unsigned_t len, cg_unsigned_t precision)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<int64_t (const char *, size_t, unsigned, unsigned), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&castStringToNumericValue, funcTy, {str, strLen, len, precision});

    return cg_i64_t( llvm::cast<llvm::Value>(result) );
}
//---------------------------------------------------------------------------
void printNumeric(unsigned len, unsigned precision, int64_t raw)
{
    long v=raw;
    if (v<0) {
        putchar('-');
        v=-v;
    }
    if (precision==0) {
        printf("%ld", v);
    } else {
        long sep=10;
        for (unsigned index=1,limit=precision;index<limit;index++)
            sep*=10;
        printf("%ld.", (v/sep));
        v=v%sep;
        if (!v) {
            for (unsigned index=0,limit=precision;index<limit;index++)
                putchar('0');
        } else {
            while (sep>(10*v)) {
                putchar('0');
                sep/=10;
            }
            printf("%ld", v);
        }
    }
}
//---------------------------------------------------------------------------
static const uint64_t msPerDay = 24*60*60*1000;
//---------------------------------------------------------------------------
static unsigned mergeTime(unsigned hour,unsigned minute,unsigned second,unsigned ms)
   // Merge into ms since midnight
{
   return ms+(1000*second)+(60*1000*minute)+(60*60*1000*hour);
}
//---------------------------------------------------------------------------
static unsigned mergeJulianDay(unsigned year, unsigned month, unsigned day)
   // Algorithm from the Calendar FAQ
{
   unsigned a = (14 - month) / 12;
   unsigned y = year + 4800 - a;
   unsigned m = month + (12*a) - 3;

   return day + ((153*m+2)/5) + (365*y) + (y/4) - (y/100) + (y/400) - 32045;
}
//---------------------------------------------------------------------------
static void splitJulianDay(unsigned jd, unsigned& year, unsigned& month, unsigned& day)
   // Algorithm from the Calendar FAQ
{
   unsigned a = jd + 32044;
   unsigned b = (4*a+3)/146097;
   unsigned c = a-((146097*b)/4);
   unsigned d = (4*c+3)/1461;
   unsigned e = c-((1461*d)/4);
   unsigned m = (5*e+2)/153;

   day = e - ((153*m+2)/5) + 1;
   month = m + 3 - (12*(m/10));
   year = (100*b) + d - 4800 + (m/10);
}
//---------------------------------------------------------------------------
void printDate(int32_t raw)
{
   unsigned year,month,day;
   splitJulianDay(raw,year,month,day);

   printf("%04u-%02u-%02u",year,month,day);
}
//---------------------------------------------------------------------------
uint32_t castStringToDateValue(const char* str,uint32_t strLen)
   // Cast a string to a date
{
   auto iter=str,limit=str+strLen;

   // Trim WS
   while ((iter!=limit)&&((*iter)==' ')) ++iter;
   while ((iter!=limit)&&((*(limit-1))==' ')) --limit;

   // Year
   unsigned year=0;
   while (true) {
      if (iter==limit) throw "invalid date format";
      char c=*(iter++);
      if (c=='-') break;
      if ((c>='0')&&(c<='9')) {
         year=10*year+(c-'0');
      } else throw "invalid date format";
   }
   // Month
   unsigned month=0;
   while (true) {
      if (iter==limit) throw "invalid date format";
      char c=*(iter++);
      if (c=='-') break;
      if ((c>='0')&&(c<='9')) {
         month=10*month+(c-'0');
      } else throw "invalid date format";
   }
   // Day
   unsigned day=0;
   while (true) {
      if (iter==limit) break;
      char c=*(iter++);
      if ((c>='0')&&(c<='9')) {
         day=10*day+(c-'0');
      } else throw "invalid date format";
   }

   // Range check
   if ((year>9999)||(month<1)||(month>12)||(day<1)||(day>31))
      throw "invalid date format";

   return mergeJulianDay(year,month,day);
}
//---------------------------------------------------------------------------
cg_u32_t genCastStringToDateValueCall(cg_ptr8_t str, cg_size_t strLen)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<uint32_t (const char *, size_t), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&castStringToDateValue, funcTy, {str, strLen});

    return cg_u32_t( llvm::cast<llvm::Value>(result) );
}
//---------------------------------------------------------------------------
uint64_t castStringToTimestampValue(const char* str,uint32_t strLen)
   // Cast a string to a timestamp value
{
//    return Timestamp(Integer::castString(str, strLen).value); // only accept integers for now
/*
    if ((strLen == 4) && (strncmp(str, "NULL", 4) == 0))
        return 0;
*/
    auto iter = str, limit = str + strLen;

    // Trim WS
    while ((iter != limit) && ((*iter) == ' ')) ++iter;
    while ((iter != limit) && ((*(limit - 1)) == ' ')) --limit;

    // Year
    unsigned year = 0;
    while (true) {
        if (iter == limit) throw "invalid timestamp format";
        char c = *(iter++);
        if (c == '-') break;
        if ((c >= '0') && (c <= '9')) {
            year = 10 * year + (c - '0');
        } else throw "invalid timestamp format";
    }
    // Month
    unsigned month = 0;
    while (true) {
        if (iter == limit) throw "invalid timestamp format";
        char c = *(iter++);
        if (c == '-') break;
        if ((c >= '0') && (c <= '9')) {
            month = 10 * month + (c - '0');
        } else throw "invalid timestamp format";
    }
    // Day
    unsigned day = 0;
    while (true) {
        if (iter == limit) break;
        char c = *(iter++);
        if (c == ' ') break;
        if ((c >= '0') && (c <= '9')) {
            day = 10 * day + (c - '0');
        } else throw "invalid timestamp format";
    }

    // Range check
    if ((year > 9999) || (month < 1) || (month > 12) || (day < 1) || (day > 31))
        throw "invalid timestamp format";
    uint64_t date = mergeJulianDay(year, month, day);

    // Hour
    unsigned hour = 0;
    while (true) {
        if (iter == limit) throw "invalid timestamp format";
        char c = *(iter++);
        if (c == ':') break;
        if ((c >= '0') && (c <= '9')) {
            hour = 10 * hour + (c - '0');
        } else throw "invalid timestamp format";
    }
    // Minute
    unsigned minute = 0;
    while (true) {
        if (iter == limit) throw "invalid timestamp format";
        char c = *(iter++);
        if (c == ':') break;
        if ((c >= '0') && (c <= '9')) {
            minute = 10 * minute + (c - '0');
        } else throw "invalid timestamp format";
    }
    // Second
    unsigned second = 0;
    while (true) {
        if (iter == limit) break;
        char c = *(iter++);
        if (c == '.') break;
        if ((c >= '0') && (c <= '9')) {
            second = 10 * second + (c - '0');
        } else throw "invalid timestamp format";
    }
    // Millisecond
    unsigned ms = 0;
    while (iter != limit) {
        char c = *(iter++);
        if ((c >= '0') && (c <= '9')) {
            ms = 10 * ms + (c - '0');
        } else throw "invalid timestamp format";
    }

    // Range check
    if ((hour >= 24) || (minute >= 60) || (second >= 60) || (ms >= 1000))
        throw "invalid timestamp format";
    uint64_t time = mergeTime(hour, minute, second, ms);

    // Merge
    return (date * msPerDay) + time;
}
//---------------------------------------------------------------------------
cg_u64_t genCastStringToTimestampValueCall(cg_ptr8_t str, cg_size_t strLen)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<uint64_t (const char *, size_t), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&castStringToTimestampValue, funcTy, {str, strLen});

    return cg_u64_t( llvm::cast<llvm::Value>(result) );
}
//---------------------------------------------------------------------------
static void splitTime(unsigned value,unsigned& hour,unsigned& minute,unsigned& second,unsigned& ms)
   // Split ms since midnight
{
   ms=value%1000; value/=1000;
   second=value%60; value/=60;
   minute=value%60; value/=60;
   hour=value%24;
}
//---------------------------------------------------------------------------
void printTimestamp(uint64_t raw)
{
   unsigned year,month,day;
   splitJulianDay(raw/msPerDay,year,month,day);
   unsigned hour,minute,second,ms;
   splitTime(raw%msPerDay,hour,minute,second,ms);

   if (ms)
      printf("%04u-%02u-%02u %u:%02u:%02u.%03u",year,month,day,hour,minute,second,ms); else
      printf("%04u-%02u-%02u %u:%02u:%02u",year,month,day,hour,minute,second);
}
