#ifndef H_Schema_hpp
#define H_Schema_hpp

#include <vector>
#include <string>
#include <unordered_map>
#include "Types.hpp"

#include <memory>

namespace SchemaParser {

struct Schema {
   struct SecondaryIndex {
      std::string name;
      std::string table;
      std::vector<unsigned> attributes;
      SecondaryIndex(const std::string & name) : name(name) { }
   };

   struct Relation {
      struct Attribute {
         std::string name;
         Types::Tag type;
         unsigned len;
         unsigned len2;
         bool notNull;
         Attribute() : len(~0), notNull(false) {}
      };
      std::string name;
      std::vector<std::unique_ptr<Schema::Relation::Attribute>> attributes;
      std::vector<unsigned> primaryKey;
      std::vector<Schema::SecondaryIndex *> secondaryIndices;
      Relation(const std::string& name) : name(name) {}
   };

   std::vector<Schema::Relation> relations;
   std::vector<Schema::SecondaryIndex> indices;
   std::unordered_multimap<std::string, Relation::Attribute *> attributes;
   std::string toString() const;
};

const SchemaParser::Schema & getSchema();

const Schema::Relation & getRelation(const std::string & name);

const Schema::Relation::Attribute * getAttribute(const std::string & name);
const Schema::Relation::Attribute * getAttribute(
   const Schema::Relation & rel, const std::string & name);

bool isAttributeName(const std::string & name);

}

#endif
