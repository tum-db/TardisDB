
#pragma once

#include <memory>
#include "Database.hpp"

/// \brief Load the database
std::unique_ptr<Database> loadDatabase();

/// \brief For testing purpose
std::unique_ptr<Database> loadUniDb();

/// \brief Load everything provided by stream into the given table
void loadTable(std::istream & stream, Table & table);
