#ifndef H_Register
#define H_Register
//---------------------------------------------------------------------------
#include <string>
//---------------------------------------------------------------------------
/// A runtime register
class Register
{
   public:
   /// Possible states
   enum class State : unsigned { Unbound, Int, Double, Bool, String };

   private:
   /// The state
   State state;
   /// Values
   union { int intValue; double doubleValue; bool boolValue; };
   /// Values
   std::string stringValue;

   public:
   /// Constructor
   Register();
   /// Destructor
   ~Register();

   /// Assign an unbound "value"
   void setUnbound() { state=State::Unbound; }
   /// Assign an integer
   void setInt(int value) { state=State::Int; intValue=value; }
   /// Assign a double value
   void setDouble(double value) { state=State::Double; doubleValue=value; }
   /// Assign a bool value
   void setBool(bool value) { state=State::Bool; boolValue=value; }
   /// Assign a string value
   void setString(const std::string& value) { state=State::String; stringValue=value; }

   /// Unbound?
   bool isUnbound() const { return state==State::Unbound; }
   /// Get the state
   State getState() const { return state; }
   /// Get the integer
   int getInt() const { return intValue; }
   /// Get the double
   double getDouble() const { return doubleValue; }
   /// Get the bool value
   bool getBool() const { return boolValue; }
   /// Get the string value
   const std::string& getString() const { return stringValue; }

   /// Comparison
   bool operator==(const Register& r) const;
   /// Comparison
   bool operator<(const Register& r) const;
   /// Hash
   unsigned getHash() const;
   /// Hash
   struct hash { unsigned operator()(const Register& r) const { return r.getHash(); }; };
};
//---------------------------------------------------------------------------
#endif
