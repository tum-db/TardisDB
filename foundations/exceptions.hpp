 
#pragma once

#include <exception>
#include <string>

class Exception : public std::exception {
public:
    Exception()
    { }

    Exception(const std::string & msg) :
            _msg(msg)
    { }

    virtual ~Exception() { };

    const char * what() const noexcept
    {
        return _msg.c_str();
    }

private:
    std::string _msg;
};

class ArithmeticException : public Exception {
public:
    ArithmeticException(const std::string & msg) :
            Exception(msg)
    { }

    ~ArithmeticException() override { };
};

class NotImplementedException : public Exception {
public:
    NotImplementedException() :
            Exception()
    { }

    NotImplementedException(const std::string & msg) :
            Exception(msg)
    { }

    ~NotImplementedException() override { };
};

class InvalidOperationException : public Exception {
public:
    InvalidOperationException(const std::string & msg) :
            Exception(msg)
    { }

    ~InvalidOperationException() override { };
};

class ParserException : public Exception {
public:
    ParserException(const std::string & msg) :
            Exception(msg)
    { }

    ~ParserException() override { };
};
