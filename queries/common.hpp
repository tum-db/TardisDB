#pragma once

#include <memory>

#include "foundations/Database.hpp"

std::unique_ptr<Database> load_tpch_db();

std::unique_ptr<Database> load_tpch_null_db();
