#include "semanticAnalyser/CopyFormat.hpp"

#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "runtime/SqlValues.hpp"

namespace semanticalAnalysis {

    /// \returns The one character of an option of COPY, \p fallback if it is not given
    static char character(const std::string & option, const std::string & name, char fallback) {
        if (option.empty()) return fallback;
        if (option.size() != 1) throw semantic_sql_error("COPY " + name + " must be a single one-byte character");
        return option[0];
    }

    CopyFormat::CopyFormat(const CopyStatement & stmt) {
        csv = (stmt.format == "csv");
        text = stmt.format.empty() || stmt.format == "text";
        header = stmt.header;
        delimiter = character(stmt.delimiter, "delimiter", csv ? ',' : text ? '\t' : '|');
        if (!csv && (!stmt.quote.empty() || !stmt.escape.empty())) {
            throw semantic_sql_error("COPY QUOTE and ESCAPE are available only in CSV mode");
        }
        quote = character(stmt.quote, "quote", '"');
        escape = character(stmt.escape, "escape", quote);
        null = (text && !stmt.nullGiven) ? "\\N" : stmt.null;
        if (text && delimiter == '\\') throw semantic_sql_error("COPY delimiter cannot be backslash");
        if (delimiter == '\n' || delimiter == '\r') throw semantic_sql_error("COPY delimiter cannot be newline or carriage return");
        if (null.find_first_of("\r\n") != std::string::npos) {
            throw semantic_sql_error("COPY null representation cannot use newline or carriage return");
        }
        if (null.find(delimiter) != std::string::npos) throw semantic_sql_error("COPY delimiter must not appear in the NULL specification");
        if (csv && delimiter == quote) throw semantic_sql_error("COPY delimiter and quote must be different");
        if (csv && null.find(quote) != std::string::npos) throw semantic_sql_error("CSV quote character must not appear in the NULL specification");
    }

    /// The value of a field of the text format: backslash escapes decoded (\n, \t, octal \123, hex \x41, ...)
    static std::string decodeText(const std::string & raw) {
        std::string value;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '\\' || i + 1 == raw.size()) { value += raw[i]; continue; }
            const char c = raw[++i];
            auto digits = [&](size_t max, int base) {
                int code = 0;
                size_t n = 0;
                for (; n < max && i < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[i])) && (base == 16 || (raw[i] >= '0' && raw[i] <= '7')); ++n, ++i) {
                    code = code * base + (std::isdigit(static_cast<unsigned char>(raw[i])) ? raw[i] - '0' : std::tolower(raw[i]) - 'a' + 10);
                }
                --i;
                return static_cast<char>(code);
            };
            switch (c) {
                case 'b': value += '\b'; break;
                case 'f': value += '\f'; break;
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                case 'v': value += '\v'; break;
                case 'x':
                    if (i + 1 < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[i + 1]))) { ++i; value += digits(2, 16); }
                    else value += c;
                    break;
                default:
                    if (c >= '0' && c <= '7') value += digits(3, 8);
                    else value += c; // \\ and an escaped delimiter
            }
        }
        return value;
    }

    bool CopyReader::next(std::vector<CopyField> & fields) {
        if (!std::getline(_in, _line)) return false;
        _recordLine = ++_lines;
        fields.clear();
        fields.emplace_back();
        if (_format.text) {
            // \. alone ends the data
            size_t end = _line.size();
            if (end > 0 && _line[end - 1] == '\r') --end;
            if (_line.compare(0, end, "\\.") == 0) return false;
            std::string raw;
            auto finish = [&]() {
                CopyField & field = fields.back();
                if (raw == _format.null) field.value = raw;
                else { field.value = decodeText(raw); field.quoted = true; }
                raw.clear();
            };
            for (size_t i = 0; i < end; ++i) {
                if (_line[i] == '\\' && i + 1 < end) { raw += _line[i]; raw += _line[++i]; continue; }
                if (_line[i] == _format.delimiter) { finish(); fields.emplace_back(); continue; }
                raw += _line[i];
            }
            finish();
            return true;
        }
        const char delimiter = _format.delimiter, quote = _format.quote, escape = _format.escape;
        bool inQuotes = false;
        for (;;) {
            // a line ends with \n or \r\n; a line break within quotes belongs to the field
            size_t end = _line.size();
            if (end > 0 && _line[end - 1] == '\r') --end;
            for (size_t i = 0; i < end; ++i) {
                const char c = _line[i];
                CopyField & field = fields.back();
                if (!_format.csv) {
                    if (c == delimiter) fields.emplace_back();
                    else field.value += c;
                } else if (inQuotes) {
                    if (c == escape && i + 1 < end && (_line[i + 1] == quote || _line[i + 1] == escape) && (escape != quote || _line[i + 1] == quote)) {
                        field.value += _line[++i];
                    } else if (c == quote) {
                        inQuotes = false;
                    } else {
                        field.value += c;
                    }
                } else if (c == delimiter) {
                    fields.emplace_back();
                } else if (c == quote) {
                    inQuotes = true;
                    field.quoted = true;
                } else {
                    field.value += c;
                }
            }
            if (!inQuotes) return true;
            if (end < _line.size()) fields.back().value += '\r';
            fields.back().value += '\n';
            if (!std::getline(_in, _line)) {
                throw semantic_sql_error("unterminated CSV quoted field (in the record of line " + std::to_string(_recordLine) + ")");
            }
            ++_lines;
        }
    }

    CopyWriter::CopyWriter(const std::string & path, const CopyFormat & format, const std::vector<std::string> & columnNames) :
            _path(path), _format(format), _columnNames(columnNames), _out(path, std::ios::out | std::ios::trunc | std::ios::binary) {
        if (!_out) throw semantic_sql_error("could not open file \"" + path + "\" for writing");
        if (_format.header) {
            _record.clear();
            for (size_t i = 0; i < _columnNames.size(); ++i) field(_columnNames[i], false, i);
            _record += '\n';
            _out << _record;
        }
    }

    void CopyWriter::field(const std::string & value, bool isNull, size_t column) {
        if (column > 0) _record += _format.delimiter;
        if (isNull) {
            _record += _format.null;
            return;
        }
        if (_format.text) {
            for (char c : value) {
                if (c == '\\') _record += "\\\\";
                else if (c == '\n') _record += "\\n";
                else if (c == '\r') _record += "\\r";
                else if (c == '\t') _record += "\\t";
                else if (c == _format.delimiter) { _record += '\\'; _record += c; }
                else _record += c;
            }
            return;
        }
        if (!_format.csv) {
            if (value.find_first_of(std::string(1, _format.delimiter) + "\r\n") != std::string::npos) {
                throw semantic_sql_error("COPY TO with FORMAT tbl cannot write a value of column \"" + _columnNames[column] +
                                         "\" that holds the delimiter or a line break; use FORMAT csv");
            }
            _record += value;
            return;
        }
        // in quotes if it holds a special character or reads as NULL
        const bool quoted = value == _format.null ||
                value.find_first_of(std::string(1, _format.delimiter) + _format.quote + _format.escape + "\r\n") != std::string::npos;
        if (!quoted) {
            _record += value;
            return;
        }
        _record += _format.quote;
        for (char c : value) {
            if (c == _format.quote || c == _format.escape) _record += _format.escape;
            _record += c;
        }
        _record += _format.quote;
    }

    void CopyWriter::write(const Native::Sql::SqlTuple & tuple) {
        _record.clear();
        for (size_t i = 0; i < tuple.values.size(); ++i) {
            const Native::Sql::Value & value = *tuple.values[i];
            const bool isNull = Native::Sql::isNullable(value) && static_cast<const Native::Sql::NullableValue &>(value).isNull();
            field(isNull ? std::string() : Native::Sql::toString(value), isNull, i);
        }
        _record += '\n';
        _out << _record;
        if (!_out) throw semantic_sql_error("could not write to file \"" + _path + "\"");
    }

    void CopyWriter::finish() {
        _out.flush();
        if (!_out) throw semantic_sql_error("could not write to file \"" + _path + "\"");
        _out.close();
    }
}
