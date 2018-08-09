#pragma once

#include <memory>
#include "Database.hpp"

std::unique_ptr<Database> load_tpch_db();

std::unique_ptr<Database> load_tpch_null_db();
