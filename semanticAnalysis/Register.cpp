#include "Register.hpp"
//---------------------------------------------------------------------------
Register::Register()
   : state(State::Unbound)
   // Constructor
{
}
//---------------------------------------------------------------------------
Register::~Register()
{
}
//---------------------------------------------------------------------------
bool Register::operator==(const Register& r) const
   // Comparison
{
   // Make sure that the state is the same
   if (state!=r.state)
      return false;

   // Compare
   switch (state) {
      case State::Unbound: return true;
      case State::Int: return intValue==r.intValue;
      case State::Double: return doubleValue==r.doubleValue;
      case State::Bool: return boolValue==r.boolValue;
      case State::String: return stringValue==r.stringValue;
   }
   return false;
}
//---------------------------------------------------------------------------
bool Register::operator<(const Register& r) const
   // Comparison
{
   // Make sure that the state is the same
   if (state!=r.state)
      return static_cast<unsigned>(state)<static_cast<unsigned>(r.state);

   // Compare
   switch (state) {
      case State::Unbound: return false;
      case State::Int: return intValue<r.intValue;
      case State::Double: return doubleValue<r.doubleValue;
      case State::Bool: return boolValue<r.boolValue;
      case State::String: return stringValue<r.stringValue;
   }
   return false;
}
//---------------------------------------------------------------------------
static uint64_t computeHash(const void* buffer,unsigned len)
   // Compute a hash value
{
   static const uint64_t FNV_offset_basis=14695981039346656037ull;
   static const uint64_t FNV_prime=1099511628211ull;

   uint64_t hash=FNV_offset_basis;
   for (const uint8_t* iter=static_cast<const uint8_t*>(buffer),*limit=iter+len;iter!=limit;++iter)
      hash=(hash^(*iter))*FNV_prime;
   return hash;
}
//---------------------------------------------------------------------------
unsigned Register::getHash() const
   // Hash
{
   switch (state) {
      case State::Unbound: return 0;
      case State::Int: return intValue;
      case State::Double: return computeHash(&doubleValue,sizeof(doubleValue));
      case State::Bool: return boolValue;
      case State::String: return computeHash(stringValue.data(),stringValue.length());
   }
   return 0;
}
//---------------------------------------------------------------------------
