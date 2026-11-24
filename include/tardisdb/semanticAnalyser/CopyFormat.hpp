// The text formats of COPY: reading and writing records

#pragma once

#include <cstdint>
#include <fstream>
#include <istream>
#include <string>
#include <vector>

#include "native/sql/SqlTuple.hpp"

namespace semanticalAnalysis {
    struct CopyStatement;

    /// A format of COPY with its options: csv, as PostgreSQL's CSV (fields in quotes may hold the delimiter,
    /// the quote and line breaks; a quote within quotes is preceded by the escape character), or tbl, the
    /// format of the TPC-H tools (fields separated by '|', without quotes; a line may end with the delimiter)
    struct CopyFormat {
        bool csv = true;
        bool header = false; // the first line names the columns
        char delimiter = ',';
        char quote = '"';
        char escape = '"';
        std::string null;    // an unquoted field with this text is NULL in a nullable column

        /// \throws semantic_sql_error if the options do not fit the format
        explicit CopyFormat(const CopyStatement & stmt);
    };

    /// A field of a record; a quoted field is never NULL
    struct CopyField {
        std::string value;
        bool quoted = false;
    };

    /// Reads the records of a file of COPY FROM, one at a time
    class CopyReader {
    public:
        CopyReader(std::istream & in, const CopyFormat & format) : _in(in), _format(format) { }

        /// Reads the next record into \p fields
        /// \returns false at the end of the input
        bool next(std::vector<CopyField> & fields);

        /// \returns The line (counted from 1) where the record read last starts
        uint64_t line() const { return _recordLine; }

    private:
        std::istream & _in;
        const CopyFormat & _format;
        std::string _line;
        uint64_t _lines = 0;
        uint64_t _recordLine = 0;
    };

    /// Writes the tuples of COPY TO into its file
    class CopyWriter {
    public:
        /// Opens (and truncates) the file and writes the names of the columns if the format has a header
        CopyWriter(const std::string & path, const CopyFormat & format, const std::vector<std::string> & columnNames);

        void write(const Native::Sql::SqlTuple & tuple);

        /// Writes what is buffered
        /// \throws semantic_sql_error if writing failed
        void finish();

    private:
        std::string _path;
        CopyFormat _format;
        std::vector<std::string> _columnNames;
        std::ofstream _out;
        std::string _record;

        void field(const std::string & value, bool isNull, size_t column);
    };
}
