#include "Schema.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>

#include "Parser.hpp"

namespace SchemaParser {

static std::string type(const Schema::Relation::Attribute& attr) {
   Types::Tag type = attr.type;
   switch(type) {
      case Types::Tag::Integer:
         return "Integer";
      case Types::Tag::Timestamp:
         return "Timestamp";
      case Types::Tag::Numeric: {
         std::stringstream ss;
         ss << "Numeric(" << attr.len << ", " << attr.len2 << ")";
         return ss.str();
      }
      case Types::Tag::Char: {
         std::stringstream ss;
         ss << "Char(" << attr.len << ")";
         return ss.str();
      }
      case Types::Tag::Varchar: {
         std::stringstream ss;
         ss << "Varchar(" << attr.len << ")";
         return ss.str();
      }
      default:
         std::cerr << "unknown type: " << (int)type << std::endl;
   }
   throw;
}

std::string Schema::toString() const {
   std::stringstream out;
   for (const Schema::Relation& rel : relations) {
      out << rel.name << std::endl;
      out << "\tPrimary Key:";
      for (unsigned keyId : rel.primaryKey)
         out << ' ' << rel.attributes[keyId]->name;
      out << std::endl;
      for (const auto& attr : rel.attributes)
         out << '\t' << attr->name << '\t' << type(*attr.get()) << (attr->notNull ? " not null" : "") << std::endl;
   }
   for (const Schema::SecondaryIndex & ind : indices) {
      out << ind.name << " on " << ind.table << "(";
      bool first = true;
      for (unsigned attr : ind.attributes) {
         if (!first) {
            out << ", ";
         }
         first = false;
         out << attr;
      }
      out << ")" << std::endl;
   }
   return out.str();
}

static SchemaParser::Schema * g_schema = nullptr;

static void loadSchema()
{
   SchemaParser::Parser p("schema.sql");
   static std::unique_ptr<SchemaParser::Schema> schema = p.parse();
   g_schema = schema.get();
}

const SchemaParser::Schema & getSchema()
{
   if (!g_schema) {
      loadSchema();
   }

   return *g_schema;
}

const Schema::Relation & getRelation(const std::string & name)
{
   const auto & schema = getSchema();
   auto it = std::find_if(schema.relations.begin(), schema.relations.end(),
      [&name](const Schema::Relation & rel) -> bool { return (rel.name.compare(name) == 0); });
   if (it == schema.relations.end()) {
      throw std::runtime_error("Relation '" + name + "' doesn't exist");
   }
   return *it;
}

const Schema::Relation::Attribute * getAttribute(
   const Schema::Relation & rel, const std::string & name)
{
   auto attr_it = std::find_if(rel.attributes.begin(), rel.attributes.end(),
      [&name](const std::unique_ptr<Schema::Relation::Attribute> & attr) -> bool
      { return (attr->name.compare(name) == 0); });
   if (attr_it == rel.attributes.end()) {
      throw std::runtime_error("Attribute '" + name + "' doesn't exist");
   }
   return attr_it->get();
}

const Schema::Relation::Attribute * getAttribute(const std::string & name)
{
   const auto & schema = getSchema();

   const size_t count = schema.attributes.count(name);
   if (count > 1) {
      throw std::runtime_error("ambiguous name: " + name);
   } else if (count == 0) {
      throw std::runtime_error("Attribute '" + name + "' doesn't exist");
   }

   auto search = schema.attributes.find(name);
   return search->second;
}

bool isAttributeName(const std::string & name)
{
   return (getSchema().attributes.count(name) > 0);
}

}
