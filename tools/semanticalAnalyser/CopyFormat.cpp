#include "semanticAnalyser/CopyFormat.hpp"

#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "native/sql/SqlValues.hpp"

namespace semanticalAnalysis {

    /// \returns The one character of an option of COPY, \p fallback if it is not given
    static char character(const std::string & option, const std::string & name, char fallback) {
        if (option.empty()) return fallback;
        if (option.size() != 1) throw semantic_sql_error("COPY " + name + " must be a single one-byte character");
        return option[0];
    }

    CopyFormat::CopyFormat(const CopyStatement & stmt) {
        if (stmt.format.empty()) {
            throw semantic_sql_error("COPY takes FORMAT csv or FORMAT tbl (PostgreSQL's text format is not supported)");
        }
        csv = (stmt.format == "csv");
        header = stmt.header;
        delimiter = character(stmt.delimiter, "delimiter", csv ? ',' : '|');
        if (!csv && (!stmt.quote.empty() || !stmt.escape.empty())) {
            throw semantic_sql_error("COPY QUOTE and ESCAPE are available only in CSV mode");
        }
        quote = character(stmt.quote, "quote", '"');
        escape = character(stmt.escape, "escape", quote);
        null = stmt.null;
        if (delimiter == '\n' || delimiter == '\r') throw semantic_sql_error("COPY delimiter cannot be newline or carriage return");
        if (null.find_first_of("\r\n") != std::string::npos) {
            throw semantic_sql_error("COPY null representation cannot use newline or carriage return");
        }
        if (null.find(delimiter) != std::string::npos) throw semantic_sql_error("COPY delimiter must not appear in the NULL specification");
        if (csv && delimiter == quote) throw semantic_sql_error("COPY delimiter and quote must be different");
        if (csv && null.find(quote) != std::string::npos) throw semantic_sql_error("CSV quote character must not appear in the NULL specification");
    }

    bool CopyReader::next(std::vector<CopyField> & fields) {
        if (!std::getline(_in, _line)) return false;
        _recordLine = ++_lines;
        fields.clear();
        fields.emplace_back();
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
