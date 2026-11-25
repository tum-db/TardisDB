// generate_series(start, stop [, step]) of integers, bigints and numerics, as in PostgreSQL

#include "queryCompiler/TableFunctions.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <regex>

#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "sql/SqlCasts.hpp"

namespace TableFunctions {

    namespace {
        using semanticalAnalysis::semantic_sql_error;

        /// The digits of a number (an integer or a numeric of the scale) as an integer scaled by 10^scale
        __int128 scaled(const std::string & text, uint8_t scale)
        {
            __int128 value = 0;
            bool negative = false, point = false;
            uint8_t fraction = 0;
            for (char c : text) {
                if (c == '-') negative = true;
                else if (c == '.') point = true;
                else if (std::isdigit(static_cast<unsigned char>(c))) {
                    if (point && fraction == scale) continue; // the analyser gives no more digits than the scale
                    value = value * 10 + (c - '0');
                    if (point) ++fraction;
                }
            }
            for (; fraction < scale; ++fraction) value *= 10;
            return negative ? -value : value;
        }

        std::string text(__int128 value, uint8_t scale)
        {
            const bool negative = value < 0;
            if (negative) value = -value;
            std::string digits;
            do { digits.insert(digits.begin(), static_cast<char>('0' + static_cast<int>(value % 10))); value /= 10; } while (value > 0);
            if (scale > 0) {
                if (digits.size() <= scale) digits.insert(0, scale + 1 - digits.size(), '0');
                digits.insert(digits.size() - scale, ".");
            }
            return negative ? "-" + digits : digits;
        }

        class GenerateSeries : public TableFunction {
        public:
            std::string name() const override { return "generate_series"; }

            std::vector<Column> columns(std::vector<Argument> & arguments) const override
            {
                if (arguments.size() != 2 && arguments.size() != 3) {
                    throw semantic_sql_error("generate_series takes (start, stop) or (start, stop, step), not " + std::to_string(arguments.size()) + " arguments");
                }
                // an unknown argument is the number it spells
                static const std::regex integer(R"(^\s*[+-]?[0-9]+\s*$)"), decimal(R"(^\s*[+-]?([0-9]+\.[0-9]*|\.[0-9]+)\s*$)");
                for (Argument & argument : arguments) {
                    if (!argument.unknown()) continue;
                    const std::string & v = argument.value;
                    if (std::regex_match(v, integer)) {
                        try {
                            long long number = std::stoll(v);
                            argument.type = (number >= INT32_MIN && number <= INT32_MAX) ? Sql::getIntegerTy(false) : Sql::getLongIntegerTy(false);
                        } catch (const std::out_of_range &) { }
                    } else if (std::regex_match(v, decimal)) {
                        const size_t digits = v.find_last_not_of(" \t") - v.find('.');
                        argument.type = Sql::getNumericTy(18, static_cast<uint8_t>(std::min<size_t>(digits, 18)), false);
                    }
                }
                // numeric if one is numeric (with the largest scale), else bigint if one is, else integer; NULL fits all
                Sql::SqlType type = Sql::getIntegerTy(false);
                uint8_t scale = 0;
                std::string names;
                bool known = true;
                for (const Argument & argument : arguments) {
                    if (argument.null) continue;
                    const Sql::SqlType & t = argument.type;
                    names += (names.empty() ? "" : ", ") + Sql::Casts::typeName(t);
                    if (t.typeID == Sql::SqlType::TypeID::NumericID) scale = std::max(scale, t.numericLayout.precision);
                    else if (t.typeID != Sql::SqlType::TypeID::IntegerID && t.typeID != Sql::SqlType::TypeID::LongIntegerID) known = false;
                }
                if (!known) throw semantic_sql_error("function generate_series(" + names + ") does not exist");
                for (const Argument & argument : arguments) {
                    if (argument.null) continue;
                    if (argument.type.typeID == Sql::SqlType::TypeID::NumericID) type = Sql::getNumericTy(18, scale, false);
                    else if (argument.type.typeID == Sql::SqlType::TypeID::LongIntegerID && type.typeID == Sql::SqlType::TypeID::IntegerID) type = Sql::getLongIntegerTy(false);
                }
                return {{"generate_series", type}};
            }

            void rows(const std::vector<Argument> & arguments, const std::vector<Column> & columns,
                      const std::function<void(std::vector<Native::Sql::value_op_t>)> & row) const override
            {
                for (const Argument & argument : arguments) if (argument.null) return; // NULL gives no rows
                const Sql::SqlType type = columns.front().type;
                const uint8_t scale = type.typeID == Sql::SqlType::TypeID::NumericID ? type.numericLayout.precision : 0;
                const __int128 start = scaled(arguments[0].value, scale), stop = scaled(arguments[1].value, scale);
                const __int128 step = arguments.size() == 3 ? scaled(arguments[2].value, scale) : scaled("1", scale);
                if (step == 0) throw semantic_sql_error("step size cannot equal zero");
                for (__int128 v = start; step > 0 ? v <= stop : v >= stop; v += step) {
                    std::vector<Native::Sql::value_op_t> values;
                    values.push_back(Native::Sql::castColumnValue(text(v, scale), type));
                    row(std::move(values));
                }
            }
        };
    }

    std::unique_ptr<TableFunction> generateSeries()
    {
        return std::make_unique<GenerateSeries>();
    }

} // end namespace TableFunctions
