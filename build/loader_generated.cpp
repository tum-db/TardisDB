#include "loader.hpp"
#include <fstream>
std::unique_ptr<Database> loadDatabase()
{
auto db = std::make_unique<Database>();
{
ModuleGen moduleGen("LoadTableModule");
auto nation = std::make_unique<Table>();
nation->addColumn("n_nationkey", Sql::getIntegerTy(false));
nation->addColumn("n_name", Sql::getCharTy(25, false));
nation->addColumn("n_regionkey", Sql::getIntegerTy(false));
nation->addColumn("n_comment", Sql::getVarcharTy(152, true));
std::ifstream fs("tables/nation.tbl");
if (!fs) { throw std::runtime_error("file not found"); }
loadTable(fs, *nation);
db->addTable(std::move(nation), "nation");
}
{
ModuleGen moduleGen("LoadTableModule");
auto region = std::make_unique<Table>();
region->addColumn("r_regionkey", Sql::getIntegerTy(false));
region->addColumn("r_name", Sql::getCharTy(25, false));
region->addColumn("r_comment", Sql::getVarcharTy(152, true));
std::ifstream fs("tables/region.tbl");
if (!fs) { throw std::runtime_error("file not found"); }
loadTable(fs, *region);
db->addTable(std::move(region), "region");
}
{
ModuleGen moduleGen("LoadTableModule");
auto part = std::make_unique<Table>();
part->addColumn("p_partkey", Sql::getIntegerTy(false));
part->addColumn("p_name", Sql::getVarcharTy(55, false));
part->addColumn("p_mfgr", Sql::getCharTy(25, false));
part->addColumn("p_brand", Sql::getCharTy(10, false));
part->addColumn("p_type", Sql::getVarcharTy(25, false));
part->addColumn("p_size", Sql::getIntegerTy(false));
part->addColumn("p_container", Sql::getCharTy(10, false));
part->addColumn("p_retailprice", Sql::getNumericTy(15, 2, false));
part->addColumn("p_comment", Sql::getVarcharTy(23, false));
std::ifstream fs("tables/part.tbl");
if (!fs) { throw std::runtime_error("file not found"); }
loadTable(fs, *part);
db->addTable(std::move(part), "part");
}
{
ModuleGen moduleGen("LoadTableModule");
auto supplier = std::make_unique<Table>();
supplier->addColumn("s_suppkey", Sql::getIntegerTy(false));
supplier->addColumn("s_name", Sql::getCharTy(25, false));
supplier->addColumn("s_address", Sql::getVarcharTy(40, false));
supplier->addColumn("s_nationkey", Sql::getIntegerTy(false));
supplier->addColumn("s_phone", Sql::getCharTy(15, false));
supplier->addColumn("s_acctbal", Sql::getNumericTy(15, 2, false));
supplier->addColumn("s_comment", Sql::getVarcharTy(101, false));
std::ifstream fs("tables/supplier.tbl");
if (!fs) { throw std::runtime_error("file not found"); }
loadTable(fs, *supplier);
db->addTable(std::move(supplier), "supplier");
}
{
ModuleGen moduleGen("LoadTableModule");
auto partsupp = std::make_unique<Table>();
partsupp->addColumn("ps_partkey", Sql::getIntegerTy(false));
partsupp->addColumn("ps_suppkey", Sql::getIntegerTy(false));
partsupp->addColumn("ps_availqty", Sql::getIntegerTy(false));
partsupp->addColumn("ps_supplycost", Sql::getNumericTy(15, 2, false));
partsupp->addColumn("ps_comment", Sql::getVarcharTy(199, false));
std::ifstream fs("tables/partsupp.tbl");
if (!fs) { throw std::runtime_error("file not found"); }
loadTable(fs, *partsupp);
db->addTable(std::move(partsupp), "partsupp");
}
{
ModuleGen moduleGen("LoadTableModule");
auto customer = std::make_unique<Table>();
customer->addColumn("c_custkey", Sql::getIntegerTy(false));
customer->addColumn("c_name", Sql::getVarcharTy(25, false));
customer->addColumn("c_address", Sql::getVarcharTy(40, false));
customer->addColumn("c_nationkey", Sql::getIntegerTy(false));
customer->addColumn("c_phone", Sql::getCharTy(15, false));
customer->addColumn("c_acctbal", Sql::getNumericTy(15, 2, false));
customer->addColumn("c_mktsegment", Sql::getCharTy(10, false));
customer->addColumn("c_comment", Sql::getVarcharTy(117, false));
std::ifstream fs("tables/customer.tbl");
if (!fs) { throw std::runtime_error("file not found"); }
loadTable(fs, *customer);
db->addTable(std::move(customer), "customer");
}
{
ModuleGen moduleGen("LoadTableModule");
auto orders = std::make_unique<Table>();
orders->addColumn("o_orderkey", Sql::getIntegerTy(false));
orders->addColumn("o_custkey", Sql::getIntegerTy(false));
orders->addColumn("o_orderstatus", Sql::getCharTy(1, false));
orders->addColumn("o_totalprice", Sql::getNumericTy(15, 2, false));
orders->addColumn("o_orderdate", Sql::getDateTy(false));
orders->addColumn("o_orderpriority", Sql::getCharTy(15, false));
orders->addColumn("o_clerk", Sql::getCharTy(15, false));
orders->addColumn("o_shippriority", Sql::getIntegerTy(false));
orders->addColumn("o_comment", Sql::getVarcharTy(79, false));
std::ifstream fs("tables/orders.tbl");
if (!fs) { throw std::runtime_error("file not found"); }
loadTable(fs, *orders);
db->addTable(std::move(orders), "orders");
}
{
ModuleGen moduleGen("LoadTableModule");
auto lineitem = std::make_unique<Table>();
lineitem->addColumn("l_orderkey", Sql::getIntegerTy(false));
lineitem->addColumn("l_partkey", Sql::getIntegerTy(false));
lineitem->addColumn("l_suppkey", Sql::getIntegerTy(false));
lineitem->addColumn("l_linenumber", Sql::getIntegerTy(true));
lineitem->addColumn("l_quantity", Sql::getNumericTy(15, 2, true));
lineitem->addColumn("l_extendedprice", Sql::getNumericTy(15, 2, true));
lineitem->addColumn("l_discount", Sql::getNumericTy(15, 2, true));
lineitem->addColumn("l_tax", Sql::getNumericTy(15, 2, true));
lineitem->addColumn("l_returnflag", Sql::getCharTy(1, false));
lineitem->addColumn("l_linestatus", Sql::getCharTy(1, false));
lineitem->addColumn("l_shipdate", Sql::getDateTy(false));
lineitem->addColumn("l_commitdate", Sql::getDateTy(false));
lineitem->addColumn("l_receiptdate", Sql::getDateTy(false));
lineitem->addColumn("l_shipinstruct", Sql::getCharTy(25, false));
lineitem->addColumn("l_shipmode", Sql::getCharTy(10, false));
lineitem->addColumn("l_comment", Sql::getVarcharTy(44, false));
std::ifstream fs("tables/lineitem.tbl");
if (!fs) { throw std::runtime_error("file not found"); }
loadTable(fs, *lineitem);
db->addTable(std::move(lineitem), "lineitem");
}
return db;
}
