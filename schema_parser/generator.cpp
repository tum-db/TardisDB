
#include "generator.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <functional>

using namespace SchemaParser;

static void generateGetSqlTypeCall(const Schema::Relation::Attribute & attr)
{
    using Type = Types::Tag;
    Type type = attr.type;

    std::string nullable = (attr.notNull ? "false" : "true");

    switch (type) {
        case Type::Integer:
            std::cout << "Sql::getIntegerTy(" << nullable << ")";
            break;
        case Type::Date:
            std::cout << "Sql::getDateTy(" << nullable << ")";
            break;
        case Type::Timestamp:
            std::cout << "Sql::getTimestampTy(" << nullable << ")";
            break;
        case Type::Numeric: {
            std::stringstream ss;
            ss << "Sql::getNumericTy(" << attr.len << ", " << attr.len2 << ", " << nullable << ")";
            std::cout << ss.str();
            break;
        }
        case Type::Char: {
            std::stringstream ss;
            ss << "Sql::getCharTy(" << attr.len << ", " << nullable << ")";
            std::cout << ss.str();
            break;
        }
        case Type::Varchar: {
            std::stringstream ss;
            ss << "Sql::getVarcharTy(" << attr.len << ", " << nullable << ")";
            std::cout << ss.str();
            break;
        }
        default:
            std::cerr << "unknown type: " << (int)type << std::endl;
    }
}

static void generateTableLoadCode(const Schema & schema, const Schema::Relation & rel)
{
    std::cout << "{\n"
                 "ModuleGen moduleGen(\"LoadTableModule\");\n";

    std::cout << "auto & " << rel.name << " = db->createTable(\"" << rel.name << "\");\n";
    for (const auto & attr : rel.attributes) {
        std::cout << rel.name << ".addColumn(\"" << attr->name << "\", ";
        generateGetSqlTypeCall(*attr);
        std::cout << ");\n";
    }

    std::cout << "std::ifstream fs(\"tables/" << rel.name << ".tbl\");\n"
              << "if (!fs) { throw std::runtime_error(\"file not found\"); }\n";

    std::cout << "loadTable(fs, " << rel.name << ");\n"
              << "}\n";
}

void generateLoadCode(const SchemaParser::Schema & schema)
{
    // intro
    std::cout << "#include \"foundations/loader.hpp\"\n"
                 "#include \"foundations/version_management.hpp\"\n" // FIXME
                 "#include <fstream>\n"
                 "std::unique_ptr<Database> loadDatabase()\n"
                 "{\n"
                 "auto db = std::make_unique<Database>();\n";

    for (const auto & rel : schema.relations) {
        generateTableLoadCode(schema, rel);
    }

    // epilog
    std::cout << "return db;\n}" << std::endl;
}
