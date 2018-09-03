#pragma once

#include <memory>

#include "foundations/Database.hpp"

std::unique_ptr<Database> load_tables();

std::unique_ptr<Database> load_tpch_null_db();
