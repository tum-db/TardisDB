// Table functions: tables from constant arguments, which a statement reads (as temporary tables) instead of the calls

#ifndef TABLEFUNCTIONS_HPP
#define TABLEFUNCTIONS_HPP

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "native/sql/SqlValues.hpp"
#include "sql/SqlType.hpp"

namespace TableFunctions {

    /// An argument of a call, evaluated
    struct Argument {
        std::string text;  // as the statement writes it
        bool null = false;
        std::string value; // the value as text, unless NULL
        Sql::SqlType type; // text for a quoted literal or a parameter of unknown type

        /// \returns Whether it is a quoted literal, whose type the function chooses (as in PostgreSQL)
        bool unknown() const;
    };

    struct Column {
        std::string name;
        Sql::SqlType type;
    };

    /// A function that returns a table from constant arguments
    class TableFunction {
    public:
        virtual ~TableFunction() = default;

        /// The name in lower case
        virtual std::string name() const = 0;

        /// \returns The columns for the arguments, whose unknown types it may set; throws if it does not take them
        virtual std::vector<Column> columns(std::vector<Argument> & arguments) const = 0;

        /// Calls \p row with the values of each row, of the types of \p columns
        virtual void rows(const std::vector<Argument> & arguments, const std::vector<Column> & columns,
                          const std::function<void(std::vector<Native::Sql::value_op_t>)> & row) const = 0;
    };

    /// \returns The table function of the name (in any case), or nullptr
    const TableFunction * find(const std::string & name);

    /// \returns The names of the table functions
    std::vector<std::string> names();

    /// Adds a table function; an existing one of its name stays. \returns Whether it was added
    bool add(std::unique_ptr<TableFunction> function);

    /// generate_series(start, stop [, step]) of integers, bigints and numerics
    std::unique_ptr<TableFunction> generateSeries();

} // end namespace TableFunctions

#endif // TABLEFUNCTIONS_HPP
