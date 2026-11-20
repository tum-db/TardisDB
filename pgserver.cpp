// A PostgreSQL-compatible server for TardisDB: frontend/backend protocol 3.0, so that psql, psycopg2,
// psycopg 3 and JDBC connect to it. One thread per connection; each connection is a session with a
// current branch (SET branch = b, or options=-c branch=b when connecting), in which the statements
// without VERSION run. Statements outside of a transaction block autocommit; a block (BEGIN ... COMMIT)
// is a transaction with snapshot isolation in a private branch (QueryCompiler::Transaction).
//
// Usage: pgserver [-h host] [-p port] [-t threads]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>

#include "foundations/Database.hpp"
#include "foundations/LegacyTypes.hpp"
#include "foundations/Parallel.hpp"
#include "foundations/exceptions.hpp"
#include "native/sql/SqlTuple.hpp"
#include "native/sql/SqlValues.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "sqlParser/SQLParser.hpp"

namespace {

constexpr int32_t protocolVersion3 = 196608;
constexpr int32_t sslRequestCode = 80877103;
constexpr int32_t gssEncRequestCode = 80877104;
constexpr int32_t cancelRequestCode = 80877102;

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string & s)
{
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

/// An error to report with an SQLSTATE
struct SqlError : std::runtime_error {
    SqlError(const std::string & sqlState, const std::string & message) : std::runtime_error(message), sqlState(sqlState) { }
    std::string sqlState;
};

// databases --------------------------------------------------------------

/// The databases of the server, by name: "tardisdb" and those of CREATE DATABASE. A session holds its
/// database, so that DROP DATABASE refuses one in use.
class Databases {
public:
    Databases() { _databases["tardisdb"] = std::make_shared<Database>(); }

    std::shared_ptr<Database> get(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _databases.find(name);
        if (it == _databases.end()) throw SqlError("3D000", "database \"" + name + "\" does not exist");
        return it->second;
    }

    void create(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_databases.count(name)) throw SqlError("42P04", "database \"" + name + "\" already exists");
        _databases[name] = std::make_shared<Database>();
    }

    void drop(const std::string & name, bool ifExists)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _databases.find(name);
        if (it == _databases.end()) {
            if (ifExists) return;
            throw SqlError("3D000", "database \"" + name + "\" does not exist");
        }
        if (it->second.use_count() > 1) {
            throw SqlError("55006", "database \"" + name + "\" is being accessed by other users");
        }
        _databases.erase(it);
    }

    std::vector<std::string> names()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<std::string> out;
        for (auto & [name, db] : _databases) out.push_back(name);
        return out;
    }

private:
    std::mutex _mutex;
    std::map<std::string, std::shared_ptr<Database>> _databases;
};

Databases databases;

// statements ----------------------------------------------------------------

/// Splits a query string at the semicolons outside literals, quoted identifiers and comments;
/// each statement keeps no semicolon
std::vector<std::string> splitStatements(const std::string & text)
{
    std::vector<std::string> statements;
    std::string current;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\'' || c == '"') {
            size_t end = i + 1;
            while (end < text.size()) {
                if (text[end] == c) {
                    if (end + 1 < text.size() && text[end + 1] == c) { end += 2; continue; } // '' or ""
                    break;
                }
                ++end;
            }
            current.append(text, i, end - i + 1);
            i = end;
        } else if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            while (i < text.size() && text[i] != '\n') ++i;
            current += '\n';
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            auto end = text.find("*/", i + 2);
            i = (end == std::string::npos) ? text.size() : end + 1;
            current += ' ';
        } else if (c == ';') {
            if (!trim(current).empty()) statements.push_back(trim(current));
            current.clear();
        } else {
            current += c;
        }
    }
    if (!trim(current).empty()) statements.push_back(trim(current));
    return statements;
}

/// Replaces the placeholders $1, $2, ... outside literals by the bound values, as SQL literals
std::string bindParameters(const std::string & query, const std::vector<std::optional<std::string>> & values)
{
    std::string out;
    for (size_t i = 0; i < query.size(); ++i) {
        char c = query[i];
        if (c == '\'' || c == '"') {
            size_t end = i + 1;
            while (end < query.size() && !(query[end] == c && (end + 1 >= query.size() || query[end + 1] != c))) {
                end += (query[end] == c) ? 2 : 1;
            }
            out.append(query, i, end - i + 1);
            i = end;
        } else if (c == '$' && i + 1 < query.size() && std::isdigit(static_cast<unsigned char>(query[i + 1]))) {
            size_t end = i + 1;
            while (end < query.size() && std::isdigit(static_cast<unsigned char>(query[end]))) ++end;
            size_t index = std::stoul(query.substr(i + 1, end - i - 1));
            if (index == 0 || index > values.size()) throw std::runtime_error("there is no parameter $" + std::to_string(index));
            const auto & value = values[index - 1];
            if (!value) {
                out += "NULL";
            } else {
                out += '\'';
                for (char v : *value) {
                    out += v;
                    if (v == '\'') out += '\'';
                }
                out += '\'';
            }
            i = end - 1;
        } else {
            out += c;
        }
    }
    return out;
}

// values ---------------------------------------------------------------------

/// The text of a parameter sent in binary, by its type OID
std::string decodeBinaryParameter(int32_t oid, const std::string & bytes)
{
    auto be = [&bytes](size_t offset, size_t n) {
        if (offset + n > bytes.size()) throw std::runtime_error("malformed binary parameter");
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v = (v << 8) | static_cast<uint8_t>(bytes[offset + i]);
        return v;
    };
    auto days = [](int64_t sinceY2k) { // days since 2000-01-01 to a Julian day, as formatDate expects
        return static_cast<uint32_t>(2451545 + sinceY2k);
    };
    switch (oid) {
        case 16: return be(0, 1) ? "true" : "false";
        case 21: return std::to_string(static_cast<int16_t>(be(0, 2)));
        case 23: return std::to_string(static_cast<int32_t>(be(0, 4)));
        case 20: return std::to_string(static_cast<int64_t>(be(0, 8)));
        case 700: { uint32_t raw = static_cast<uint32_t>(be(0, 4)); float f; memcpy(&f, &raw, 4); return std::to_string(f); }
        case 701: { uint64_t raw = be(0, 8); double d; memcpy(&d, &raw, 8); std::ostringstream s; s.precision(17); s << d; return s.str(); }
        case 25: case 1042: case 1043: case 19: case 705: case 0: return bytes;
        case 1082: return formatDate(days(static_cast<int32_t>(be(0, 4))));
        case 1114: case 1184: {
            int64_t micros = static_cast<int64_t>(be(0, 8));
            int64_t ms = micros / 1000;
            int64_t day = ms / (24 * 3600 * 1000);
            int64_t rest = ms % (24 * 3600 * 1000);
            if (rest < 0) { rest += 24 * 3600 * 1000; --day; }
            return formatTimestamp(static_cast<uint64_t>(days(day)) * 24 * 3600 * 1000 + static_cast<uint64_t>(rest));
        }
        case 1700: { // numeric: ndigits, weight, sign, dscale, base-10000 digits
            int16_t ndigits = static_cast<int16_t>(be(0, 2)), weight = static_cast<int16_t>(be(2, 2));
            uint16_t sign = static_cast<uint16_t>(be(4, 2)), dscale = static_cast<uint16_t>(be(6, 2));
            if (sign == 0xC000) return "NaN";
            std::string integer, fraction;
            for (int16_t i = 0; i < ndigits; ++i) {
                unsigned digit = static_cast<unsigned>(be(8 + 2 * i, 2));
                char group[5];
                snprintf(group, sizeof(group), "%04u", digit);
                if (i <= weight) integer += group; else fraction += group;
            }
            for (int16_t i = ndigits; i <= weight; ++i) integer += "0000";
            if (weight < -1) fraction.insert(0, static_cast<size_t>(4 * (-1 - weight)), '0');
            integer.erase(0, std::min(integer.find_first_not_of('0'), integer.size()));
            if (integer.empty()) integer = "0";
            fraction.resize(dscale, '0');
            return (sign == 0x4000 ? "-" : "") + integer + (dscale ? "." + fraction : "");
        }
        default: throw SqlError("0A000", "binary parameters of type " + std::to_string(oid) + " are not supported");
    }
}

/// Type OID and length of a column, as in pg_type
std::pair<int32_t, int16_t> pgType(const Sql::SqlType & type)
{
    using T = Sql::SqlType::TypeID;
    switch (type.typeID) {
        case T::BoolID: return {16, 1};
        case T::IntegerID: return {23, 4};
        case T::LongIntegerID: return {20, 8};
        case T::NumericID: return {1700, -1};
        case T::CharID: return {1042, -1};
        case T::VarcharID: return {1043, -1};
        case T::TextID: return {25, -1};
        case T::DateID: return {1082, 4};
        case T::TimestampID: return {1114, 8};
        default: return {705, -2}; // unknown
    }
}

std::string formatNumeric(int64_t raw, unsigned scale)
{
    uint64_t magnitude = raw < 0 ? 0 - static_cast<uint64_t>(raw) : static_cast<uint64_t>(raw);
    std::string digits = std::to_string(magnitude);
    if (scale > 0) {
        if (digits.size() <= scale) digits.insert(0, scale - digits.size() + 1, '0');
        digits.insert(digits.size() - scale, ".");
    }
    return (raw < 0 ? "-" : "") + digits;
}

/// The text format of PostgreSQL; nullopt is NULL (TardisDB passes no value for NULL)
std::optional<std::string> formatValue(const Native::Sql::Value * value, const Sql::SqlType & type)
{
    using T = Sql::SqlType::TypeID;
    if (value == nullptr) return std::nullopt;
    if (auto * nullable = dynamic_cast<const Native::Sql::NullableValue *>(value)) {
        if (nullable->isNull()) return std::nullopt;
        value = &nullable->getValue();
    }
    switch (type.typeID) {
        case T::BoolID: return std::string(static_cast<const Native::Sql::Bool *>(value)->value ? "t" : "f");
        case T::IntegerID: return std::to_string(static_cast<const Native::Sql::Integer *>(value)->value);
        case T::LongIntegerID: return std::to_string(static_cast<const Native::Sql::Numeric *>(value)->value);
        case T::NumericID:
            return formatNumeric(static_cast<const Native::Sql::Numeric *>(value)->value, type.numericLayout.precision);
        case T::CharID:
        case T::VarcharID: {
            auto * string = static_cast<const Native::Sql::Varchar *>(value);
            std::string s(string->value, string->len);
            s.erase(std::find(s.begin(), s.end(), '\0'), s.end());
            return s;
        }
        case T::TextID: {
            auto * text = static_cast<const Native::Sql::Text *>(value);
            const uintptr_t * raw = text->value.data();
            uintptr_t start = raw[0];
            if ((start >> (8 * sizeof(uintptr_t) - 1)) == 0) {
                const uint8_t * data = reinterpret_cast<const uint8_t *>(raw);
                return std::string(reinterpret_cast<const char *>(&data[1]), data[0]);
            }
            start ^= static_cast<uintptr_t>(1) << (8 * sizeof(uintptr_t) - 1);
            return std::string(reinterpret_cast<const char *>(start), raw[1] - start);
        }
        case T::DateID: return formatDate(static_cast<const Native::Sql::Date *>(value)->value);
        case T::TimestampID: return formatTimestamp(static_cast<const Native::Sql::Timestamp *>(value)->value);
        default: throw std::runtime_error("cannot send a value of this type");
    }
}

// protocol -------------------------------------------------------------------

/// A backend message under construction
class Message {
public:
    explicit Message(char type) : _type(type) { }

    Message & int32(int32_t v) { uint32_t n = htonl(static_cast<uint32_t>(v)); _body.append(reinterpret_cast<char *>(&n), 4); return *this; }
    Message & int16(int16_t v) { uint16_t n = htons(static_cast<uint16_t>(v)); _body.append(reinterpret_cast<char *>(&n), 2); return *this; }
    Message & byte(char c) { _body += c; return *this; }
    Message & string(const std::string & s) { _body += s; _body += '\0'; return *this; }
    Message & bytes(const std::string & s) { _body += s; return *this; }

    std::string encode() const
    {
        std::string out(1, _type);
        uint32_t n = htonl(static_cast<uint32_t>(_body.size() + 4));
        out.append(reinterpret_cast<char *>(&n), 4);
        return out + _body;
    }

private:
    char _type;
    std::string _body;
};

/// Reads a frontend message body
class Reader {
public:
    explicit Reader(const std::string & body) : _body(body) { }

    int32_t int32() { uint32_t n; need(4); memcpy(&n, _body.data() + _pos, 4); _pos += 4; return static_cast<int32_t>(ntohl(n)); }
    int16_t int16() { uint16_t n; need(2); memcpy(&n, _body.data() + _pos, 2); _pos += 2; return static_cast<int16_t>(ntohs(n)); }
    char byte() { need(1); return _body[_pos++]; }
    std::string string()
    {
        auto end = _body.find('\0', _pos);
        if (end == std::string::npos) throw std::runtime_error("malformed message");
        std::string s = _body.substr(_pos, end - _pos);
        _pos = end + 1;
        return s;
    }
    std::string bytes(size_t n) { need(n); std::string s = _body.substr(_pos, n); _pos += n; return s; }

private:
    void need(size_t n) const { if (_pos + n > _body.size()) throw std::runtime_error("malformed message"); }

    const std::string & _body;
    size_t _pos = 0;
};

/// The outcome of one statement, buffered until it is sent
struct Outcome {
    bool returnsRows = false;
    std::vector<std::string> columnNames;
    std::vector<Sql::SqlType> columnTypes;
    std::vector<std::vector<std::optional<std::string>>> rows;
    std::string tag;   // the CommandComplete tag
    bool empty = false; // an empty query
    std::string notice; // sent as a NoticeResponse before CommandComplete
};

class Session {
public:
    explicit Session(int fd) : _fd(fd) { }

    ~Session()
    {
        // a connection that ends in a transaction block rolls it back
        if (_transaction) {
            try {
                QueryCompiler::rollbackTransaction(*_db, *_transaction);
            } catch (const std::exception & e) {
                std::cerr << "pgserver: rollback at the end of the connection failed: " << e.what() << std::endl;
            }
        }
    }

    void run()
    {
        try {
            if (!startup()) return;
            for (;;) {
                char type;
                std::string body;
                if (!readMessage(type, body)) return;
                if (type == 'X') return;
                handle(type, body);
            }
        } catch (const std::exception & e) {
            std::cerr << "pgserver: connection closed: " << e.what() << std::endl;
        }
    }

private:
    // connection ---------------------------------------------------------

    bool readFully(void * buffer, size_t n)
    {
        auto * p = static_cast<char *>(buffer);
        while (n > 0) {
            ssize_t r = ::recv(_fd, p, n, 0);
            if (r <= 0) return false;
            p += r;
            n -= static_cast<size_t>(r);
        }
        return true;
    }

    bool readMessage(char & type, std::string & body)
    {
        if (!readFully(&type, 1)) return false;
        uint32_t length;
        if (!readFully(&length, 4)) return false;
        length = ntohl(length);
        if (length < 4) throw std::runtime_error("malformed message length");
        body.resize(length - 4);
        return body.empty() || readFully(body.data(), body.size());
    }

    void send(const std::string & bytes)
    {
        size_t sent = 0;
        while (sent < bytes.size()) {
            ssize_t r = ::send(_fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
            if (r <= 0) throw std::runtime_error("client gone");
            sent += static_cast<size_t>(r);
        }
    }

    void queue(const Message & message) { _out += message.encode(); }
    void flush() { send(_out); _out.clear(); }

    bool startup()
    {
        for (;;) {
            uint32_t length;
            if (!readFully(&length, 4)) return false;
            length = ntohl(length);
            if (length < 8 || length > 10000) return false;
            std::string body(length - 4, '\0');
            if (!readFully(body.data(), body.size())) return false;
            Reader reader(body);
            int32_t code = reader.int32();
            if (code == sslRequestCode || code == gssEncRequestCode) {
                send("N"); // no encryption; the client goes on in plain text or gives up
                continue;
            }
            if (code == cancelRequestCode) return false; // statements cannot be cancelled
            if (code != protocolVersion3) {
                queue(errorMessage("FATAL", "0A000", "unsupported frontend protocol"));
                flush();
                return false;
            }
            std::map<std::string, std::string> parameters;
            for (;;) {
                std::string key = reader.string();
                if (key.empty()) break;
                parameters[key] = reader.string();
            }
            std::string database = parameters.count("database") ? parameters["database"] : parameters["user"];
            if (database.empty()) database = "tardisdb";
            try {
                _dbHandle = databases.get(database);
            } catch (const SqlError & e) {
                queue(errorMessage("FATAL", e.sqlState, e.what()));
                flush();
                return false;
            }
            _db = _dbHandle.get();
            _database = database;
            // options: -c name=value ...
            std::string options = parameters["options"];
            for (size_t pos = 0; (pos = options.find("-c", pos)) != std::string::npos;) {
                pos += 2;
                while (pos < options.size() && options[pos] == ' ') ++pos;
                auto end = options.find(' ', pos);
                std::string setting = options.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                auto eq = setting.find('=');
                if (eq != std::string::npos && isBranchSetting(setting.substr(0, eq))) {
                    try {
                        setBranch(setting.substr(eq + 1));
                    } catch (const SqlError & e) {
                        queue(errorMessage("FATAL", e.sqlState, e.what()));
                        flush();
                        return false;
                    }
                }
                pos = (end == std::string::npos) ? options.size() : end;
            }

            queue(Message('R').int32(0)); // AuthenticationOk: no password
            for (auto & [name, value] : std::vector<std::pair<std::string, std::string>>{
                    {"server_version", "16.0 (TardisDB)"}, {"server_encoding", "UTF8"}, {"client_encoding", "UTF8"},
                    {"DateStyle", "ISO, MDY"}, {"IntervalStyle", "postgres"}, {"integer_datetimes", "on"},
                    {"standard_conforming_strings", "on"}, {"TimeZone", "UTC"}, {"is_superuser", "on"},
                    {"session_authorization", parameters["user"]}, {"application_name", parameters["application_name"]}}) {
                queue(Message('S').string(name).string(value));
            }
            queue(Message('K').int32(static_cast<int32_t>(::getpid())).int32(_fd));
            readyForQuery();
            flush();
            return true;
        }
    }

    // messages -----------------------------------------------------------

    Message errorMessage(const std::string & severity, const std::string & sqlState, const std::string & text) const
    {
        Message m('E');
        m.byte('S').string(severity).byte('V').string(severity).byte('C').string(sqlState).byte('M').string(text).byte(0);
        return m;
    }

    void readyForQuery()
    {
        queue(Message('Z').byte(_failedBlock ? 'E' : (_inBlock ? 'T' : 'I')));
    }

    void rowDescription(const Outcome & outcome)
    {
        Message m('T');
        m.int16(static_cast<int16_t>(outcome.columnNames.size()));
        for (size_t i = 0; i < outcome.columnNames.size(); ++i) {
            auto [oid, length] = pgType(outcome.columnTypes[i]);
            m.string(outcome.columnNames[i]).int32(0).int16(0).int32(oid).int16(length).int32(-1).int16(0);
        }
        queue(m);
    }

    void dataRows(const Outcome & outcome, size_t from, size_t to)
    {
        for (size_t r = from; r < to; ++r) {
            Message m('D');
            m.int16(static_cast<int16_t>(outcome.rows[r].size()));
            for (auto & value : outcome.rows[r]) {
                if (!value) {
                    m.int32(-1);
                } else {
                    m.int32(static_cast<int32_t>(value->size())).bytes(*value);
                }
            }
            queue(m);
        }
    }

    void commandComplete(const Outcome & outcome)
    {
        if (!outcome.notice.empty()) {
            Message m('N');
            m.byte('S').string("NOTICE").byte('V').string("NOTICE").byte('C').string("00000").byte('M').string(outcome.notice).byte(0);
            queue(m);
        }
        if (outcome.empty) {
            queue(Message('I')); // EmptyQueryResponse
        } else {
            queue(Message('C').string(outcome.tag));
        }
    }

    void reportError(const std::exception & e)
    {
        std::string state = "XX000";
        if (auto * s = dynamic_cast<const SqlError *>(&e)) state = s->sqlState;
        else if (auto * t = dynamic_cast<const QueryCompiler::TransactionError *>(&e)) state = t->sqlState;
        else if (dynamic_cast<const tardisParser::syntactical_error *>(&e)) state = "42601";
        else if (dynamic_cast<const semanticalAnalysis::not_null_violation *>(&e)) state = "23502";
        else if (dynamic_cast<const semanticalAnalysis::semantic_sql_error *>(&e)) state = "42000";
        else if (dynamic_cast<const NotImplementedException *>(&e)) state = "0A000";
        queue(errorMessage("ERROR", state, e.what()));
        if (_inBlock) _failedBlock = true;
    }

    // statements -----------------------------------------------------------

    static bool isBranchSetting(const std::string & name)
    {
        auto n = lower(trim(name));
        return n == "branch" || n == "tardisdb.branch";
    }

    void setBranch(std::string name)
    {
        name = trim(name);
        if (name.size() >= 2 && (name.front() == '\'' || name.front() == '"') && name.back() == name.front()) {
            name = name.substr(1, name.size() - 2);
        }
        if (name != "master" && name != "main" && _db->_branchMapping.count(name) == 0) {
            throw SqlError("3D000", "branch \"" + name + "\" does not exist");
        }
        _branch = name;
    }

    /// Session statements (transactions, settings) that the engine does not know; nullopt for all others
    std::optional<Outcome> sessionStatement(const std::string & statement)
    {
        std::string s = lower(statement);
        std::vector<std::string> words;
        for (size_t pos = 0; pos < s.size();) {
            while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
            size_t end = pos;
            while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end]))) ++end;
            if (end > pos) words.push_back(s.substr(pos, end - pos));
            pos = end;
        }
        if (words.empty()) return std::nullopt;
        Outcome outcome;
        const std::string & first = words[0];
        if (first == "begin" || (first == "start" && words.size() > 1 && words[1] == "transaction")) {
            if (!_inBlock) { // as PostgreSQL, a BEGIN in a block changes nothing
                _transaction = QueryCompiler::beginTransaction(*_db, _branch);
                _inBlock = true;
            }
            outcome.tag = "BEGIN";
        } else if (first == "commit" || first == "end" || first == "rollback" || first == "abort") {
            bool commit = (first == "commit" || first == "end") && !_failedBlock; // as PostgreSQL, a failed block rolls back
            auto transaction = std::move(_transaction);
            _transaction.reset();
            _inBlock = _failedBlock = false;
            if (transaction) {
                // a commit that conflicts is an error and rolls back
                if (commit) QueryCompiler::commitTransaction(*_db, *transaction);
                else QueryCompiler::rollbackTransaction(*_db, *transaction);
            }
            outcome.tag = commit ? "COMMIT" : "ROLLBACK";
        } else if (first == "set") {
            static const std::regex set(R"(^\s*set\s+(?:session\s+|local\s+)?([a-z_][\w.]*)\s*(?:=|\s+to\s+)\s*(.*?)\s*$)", std::regex::icase);
            std::smatch match;
            if (!std::regex_match(statement, match, set)) throw SqlError("42601", "syntax error in SET");
            if (isBranchSetting(match[1].str())) {
                if (_inBlock) throw SqlError("25001", "SET branch cannot run inside a transaction block");
                setBranch(match[2].str());
            }
            outcome.tag = "SET"; // other settings are accepted and have no effect
        } else if (first == "show" && words.size() == 2 && words[1] != "databases" && words[1] != "tables" && words[1] != "branches") {
            std::string name = words[1];
            std::string value = isBranchSetting(name) ? _branch
                    : name == "server_version" ? "16.0 (TardisDB)"
                    : name == "transaction_isolation" || name == "default_transaction_isolation" ? "repeatable read"
                    : name == "standard_conforming_strings" ? "on"
                    : name == "client_encoding" || name == "server_encoding" ? "UTF8"
                    : name == "timezone" ? "UTC" : name == "datestyle" ? "ISO, MDY" : "";
            outcome.returnsRows = true;
            outcome.columnNames = {name};
            outcome.columnTypes = {Sql::getTextTy(false)};
            outcome.rows.push_back({value});
            outcome.tag = "SHOW";
        } else if ((first == "create" || first == "drop") && words.size() >= 3 && words[1] == "database") {
            // CREATE DATABASE name, DROP DATABASE [IF EXISTS] name
            bool ifExists = first == "drop" && words.size() >= 5 && words[2] == "if" && words[3] == "exists";
            std::string name = trim(statement.substr(statement.size() - words.back().size()));
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
            if (first == "create") databases.create(name); else databases.drop(name, ifExists);
            outcome.tag = first == "create" ? "CREATE DATABASE" : "DROP DATABASE";
        } else if (first == "show" && words.size() >= 2 && (words[1] == "databases" || words[1] == "tables" ||
                                                              words[1] == "branches" || words[1] == "columns")) {
            catalog(outcome, words, statement);
        } else if (first == "discard" || (first == "reset" && words.size() > 1 && words[1] == "all")) {
            if (_inBlock) throw SqlError("25001", lower(trim(statement)) + " cannot run inside a transaction block");
            _branch = "master";
            outcome.tag = first == "discard" ? "DISCARD ALL" : "RESET";
        } else if (s == "select version()" || s == "select current_database()" || s == "select current_schema()") {
            outcome.returnsRows = true;
            outcome.columnNames = {s.substr(7, s.find('(') - 7)};
            outcome.columnTypes = {Sql::getTextTy(false)};
            outcome.rows.push_back({s == "select version()" ? std::string("PostgreSQL 16.0 (TardisDB)")
                                   : s == "select current_database()" ? _database : std::string("public")});
            outcome.tag = "SELECT 1";
        } else {
            return std::nullopt;
        }
        return outcome;
    }

    /// The type as PostgreSQL names it in information_schema.columns.data_type, with its length
    static std::string typeName(const Sql::SqlType & type)
    {
        using T = Sql::SqlType::TypeID;
        switch (type.typeID) {
            case T::BoolID: return "boolean";
            case T::IntegerID: return "integer";
            case T::LongIntegerID: return "bigint";
            case T::NumericID: return "numeric(" + std::to_string(type.numericLayout.length) + "," + std::to_string(type.numericLayout.precision) + ")";
            case T::CharID: return "character(" + std::to_string(type.length) + ")";
            case T::VarcharID: return "character varying(" + std::to_string(type.length) + ")";
            case T::TextID: return "text";
            case T::DateID: return "date";
            case T::TimestampID: return "timestamp without time zone";
            default: return "unknown";
        }
    }

    /// SHOW DATABASES | TABLES | BRANCHES | COLUMNS FROM table: the catalog of TardisDB, which has no
    /// information_schema; COLUMNS are those of the session's branch, with their position in the key
    void catalog(Outcome & outcome, const std::vector<std::string> & words, const std::string & statement)
    {
        auto text = Sql::getTextTy(false);
        auto integer = Sql::getIntegerTy(false);
        outcome.returnsRows = true;
        std::shared_lock<SharedLatch> latch(_db->_catalogLatch);
        if (words[1] == "databases") {
            outcome.columnNames = {"name"};
            outcome.columnTypes = {text};
            for (auto & name : databases.names()) outcome.rows.push_back({name});
        } else if (words[1] == "tables") {
            outcome.columnNames = {"table_name"};
            outcome.columnTypes = {text};
            std::vector<std::string> names;
            for (auto & [name, table] : _db->getTables()) names.push_back(name);
            std::sort(names.begin(), names.end());
            for (auto & name : names) outcome.rows.push_back({name});
        } else if (words[1] == "branches") {
            outcome.columnNames = {"branch_name"};
            outcome.columnTypes = {text};
            std::vector<std::string> names;
            for (auto & [name, id] : _db->_branchMapping) if (name != "main" && !_db->_branches.at(id)->transaction) names.push_back(name);
            std::sort(names.begin(), names.end());
            for (auto & name : names) outcome.rows.push_back({name});
        } else {
            if (words.size() != 4 || words[2] != "from") throw SqlError("42601", "SHOW COLUMNS FROM table");
            std::string tableName = trim(statement.substr(statement.size() - words[3].size()));
            Table * table = _db->getTable(tableName);
            if (table == nullptr) throw SqlError("42P01", "relation \"" + tableName + "\" does not exist");
            const std::string & name = _transaction ? _transaction->snapshot : _branch;
            branch_id_t branch = _db->_branchMapping.at(name);
            const auto & key = table->getPrimaryKey();
            outcome.columnNames = {"column_name", "data_type", "is_nullable", "key_position"};
            outcome.columnTypes = {text, text, text, integer};
            for (auto & name : table->getColumnNames(branch)) {
                ci_p_t ci = table->findColumn(name, branch);
                auto position = std::find(key.begin(), key.end(), name);
                outcome.rows.push_back({name, typeName(ci->type), std::string(ci->type.nullable ? "YES" : "NO"),
                                        position == key.end() ? std::string("0") : std::to_string(position - key.begin() + 1)});
            }
        }
        outcome.tag = "SHOW";
    }

    Outcome execute(const std::string & statement)
    {
        if (trim(statement).empty()) {
            Outcome outcome;
            outcome.empty = true;
            return outcome;
        }
        if (_failedBlock) {
            auto w = lower(trim(statement));
            if (w.rfind("rollback", 0) != 0 && w.rfind("abort", 0) != 0 && w.rfind("commit", 0) != 0 && w.rfind("end", 0) != 0) {
                throw SqlError("25P02", "current transaction is aborted, commands ignored until end of transaction block");
            }
        }
        if (auto outcome = sessionStatement(statement)) return *outcome;

        Outcome outcome;
        QueryCompiler::StatementResult result;
        try {
        const QueryCompiler::Transaction * transaction = _transaction ? &*_transaction : nullptr;
        result = QueryCompiler::execute(statement + ";", *_db, _branch, [&outcome](Native::Sql::SqlTuple * tuple) {
            std::vector<std::optional<std::string>> row;
            row.reserve(tuple->values.size());
            for (size_t i = 0; i < tuple->values.size(); ++i) {
                row.push_back(formatValue(tuple->values[i].get(), outcome.columnTypes[i]));
            }
            outcome.rows.push_back(std::move(row));
        }, [&outcome](const QueryCompiler::StatementResult & described) {
            // the columns are known before the first tuple arrives
            outcome.columnNames = described.columnNames;
            outcome.columnTypes = described.columnTypes;
        }, transaction);
        } catch (const char * message) {
            throw SqlError("22000", message); // parts of TardisDB throw C strings, e.g. for invalid values
        } catch (const std::exception &) {
            throw;
        } catch (...) {
            throw SqlError("XX000", "internal error");
        }
        outcome.returnsRows = result.returnsRows;
        outcome.notice = result.notice;
        outcome.columnNames = result.columnNames;
        outcome.columnTypes = result.columnTypes;
        if (result.tag == "SELECT") outcome.tag = "SELECT " + std::to_string(result.rows);
        else if (result.tag == "INSERT") outcome.tag = "INSERT 0 " + std::to_string(result.rows);
        else if (result.tag == "UPDATE" || result.tag == "DELETE" || result.tag == "COPY" || result.tag == "MERGE") outcome.tag = result.tag + " " + std::to_string(result.rows);
        else outcome.tag = result.tag.empty() ? "OK" : result.tag;
        return outcome;
    }

    void handle(char type, const std::string & body)
    {
        if (_skipUntilSync && type != 'S') return; // after an error in the extended protocol
        Reader reader(body);
        switch (type) {
            case 'Q': { // simple query
                std::string text = reader.string();
                auto statements = splitStatements(text);
                if (statements.empty()) statements.push_back("");
                for (auto & statement : statements) {
                    try {
                        Outcome outcome = execute(statement);
                        if (outcome.returnsRows) rowDescription(outcome);
                        dataRows(outcome, 0, outcome.rows.size());
                        commandComplete(outcome);
                    } catch (const std::exception & e) {
                        reportError(e);
                        break; // the rest of the query string is not executed
                    }
                }
                readyForQuery();
                flush();
                break;
            }
            case 'P': { // Parse
                try {
                    std::string name = reader.string();
                    std::string query = reader.string();
                    int16_t n = reader.int16();
                    Prepared prepared;
                    prepared.query = trim(query);
                    while (!prepared.query.empty() && prepared.query.back() == ';') {
                        prepared.query = trim(prepared.query.substr(0, prepared.query.size() - 1));
                    }
                    if (splitStatements(prepared.query).size() > 1) {
                        throw SqlError("42601", "cannot insert multiple commands into a prepared statement");
                    }
                    for (int16_t i = 0; i < n; ++i) prepared.parameterTypes.push_back(reader.int32());
                    _statements[name] = prepared;
                    queue(Message('1'));
                } catch (const std::exception & e) {
                    extendedError(e);
                }
                break;
            }
            case 'B': { // Bind
                try {
                    std::string portalName = reader.string();
                    std::string statementName = reader.string();
                    auto it = _statements.find(statementName);
                    if (it == _statements.end()) throw SqlError("26000", "prepared statement \"" + statementName + "\" does not exist");
                    int16_t formats = reader.int16();
                    std::vector<int16_t> formatCodes;
                    for (int16_t i = 0; i < formats; ++i) formatCodes.push_back(reader.int16());
                    int16_t n = reader.int16();
                    std::vector<std::optional<std::string>> values;
                    for (int16_t i = 0; i < n; ++i) {
                        int16_t format = formatCodes.empty() ? 0 : formatCodes[formatCodes.size() == 1 ? 0 : i];
                        int32_t length = reader.int32();
                        if (length < 0) {
                            values.emplace_back(std::nullopt);
                            continue;
                        }
                        std::string bytes = reader.bytes(static_cast<size_t>(length));
                        if (format == 0) {
                            values.emplace_back(std::move(bytes));
                        } else {
                            auto & types = it->second.parameterTypes;
                            values.emplace_back(decodeBinaryParameter(i < static_cast<int16_t>(types.size()) ? types[i] : 0, bytes));
                        }
                    }
                    int16_t resultFormats = reader.int16();
                    for (int16_t i = 0; i < resultFormats; ++i) {
                        if (reader.int16() != 0) throw SqlError("0A000", "binary results are not supported");
                    }
                    Portal portal;
                    portal.query = bindParameters(it->second.query, values);
                    _portals[portalName] = std::move(portal);
                    queue(Message('2'));
                } catch (const std::exception & e) {
                    extendedError(e);
                }
                break;
            }
            case 'D': { // Describe
                try {
                    char kind = reader.byte();
                    std::string name = reader.string();
                    if (kind == 'S') {
                        auto it = _statements.find(name);
                        if (it == _statements.end()) throw SqlError("26000", "prepared statement \"" + name + "\" does not exist");
                        Message m('t');
                        m.int16(static_cast<int16_t>(it->second.parameterTypes.size()));
                        for (int32_t oid : it->second.parameterTypes) m.int32(oid == 0 ? 25 : oid); // unknown: text
                        queue(m);
                        queue(Message('n')); // the columns are known only once it runs
                    } else {
                        Portal & portal = findPortal(name);
                        runPortal(portal); // TardisDB describes a statement by compiling it
                        if (portal.outcome.returnsRows) rowDescription(portal.outcome);
                        else queue(Message('n'));
                    }
                } catch (const std::exception & e) {
                    extendedError(e);
                }
                break;
            }
            case 'E': { // Execute
                try {
                    std::string name = reader.string();
                    int32_t maxRows = reader.int32();
                    Portal & portal = findPortal(name);
                    runPortal(portal);
                    size_t to = portal.outcome.rows.size();
                    if (maxRows > 0) to = std::min(to, portal.sent + static_cast<size_t>(maxRows));
                    dataRows(portal.outcome, portal.sent, to);
                    portal.sent = to;
                    if (portal.sent < portal.outcome.rows.size()) {
                        queue(Message('s')); // PortalSuspended
                    } else {
                        commandComplete(portal.outcome);
                    }
                } catch (const std::exception & e) {
                    extendedError(e);
                }
                break;
            }
            case 'C': { // Close
                char kind = reader.byte();
                std::string name = reader.string();
                if (kind == 'S') _statements.erase(name); else _portals.erase(name);
                queue(Message('3'));
                break;
            }
            case 'S': // Sync
                _skipUntilSync = false;
                _portals.erase("");
                readyForQuery();
                flush();
                break;
            case 'H': // Flush
                flush();
                break;
            default:
                queue(errorMessage("ERROR", "08P01", std::string("unsupported message type ") + type));
                readyForQuery();
                flush();
                break;
        }
    }

    struct Prepared {
        std::string query;
        std::vector<int32_t> parameterTypes;
    };

    struct Portal {
        std::string query;
        bool executed = false;
        Outcome outcome;
        size_t sent = 0;
    };

    Portal & findPortal(const std::string & name)
    {
        auto it = _portals.find(name);
        if (it == _portals.end()) throw SqlError("34000", "portal \"" + name + "\" does not exist");
        return it->second;
    }

    void runPortal(Portal & portal)
    {
        if (!portal.executed) {
            portal.outcome = execute(portal.query);
            portal.executed = true;
        }
    }

    void extendedError(const std::exception & e)
    {
        reportError(e);
        _skipUntilSync = true;
    }

    int _fd;
    std::shared_ptr<Database> _dbHandle;
    Database * _db = nullptr;
    std::string _database;
    std::string _branch = "master";
    bool _inBlock = false;     // between BEGIN and COMMIT
    bool _failedBlock = false; // a statement of the block failed
    std::optional<QueryCompiler::Transaction> _transaction; // of the block
    bool _skipUntilSync = false;
    std::map<std::string, Prepared> _statements;
    std::map<std::string, Portal> _portals;
    std::string _out;
};

} // end anonymous namespace

int main(int argc, char ** argv)
{
    std::string host = "127.0.0.1";
    int port = 5432;
    int opt;
    while ((opt = getopt(argc, argv, "h:p:t:")) != -1) {
        switch (opt) {
            case 'h': host = optarg; break;
            case 'p': port = atoi(optarg); break;
            case 't': Parallel::threads = std::max(1, atoi(optarg)); break;
            default:
                std::cerr << "usage: " << argv[0] << " [-h host] [-p port] [-t threads]" << std::endl;
                return 1;
        }
    }
    // necessary for the LLVM JIT compiler
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        std::cerr << "pgserver: invalid host " << host << std::endl;
        return 1;
    }
    if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || ::listen(listener, 64) != 0) {
        std::cerr << "pgserver: cannot listen on " << host << ":" << port << ": " << strerror(errno) << std::endl;
        return 1;
    }
    std::cerr << "pgserver: listening on " << host << ":" << port << " with " << Parallel::threads << " threads per query" << std::endl;

    for (;;) {
        int fd = ::accept(listener, nullptr, nullptr);
        if (fd < 0) continue;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        std::thread([fd]() {
            Session(fd).run();
            ::close(fd);
        }).detach();
    }
}
