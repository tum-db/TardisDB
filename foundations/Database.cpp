#include "foundations/Database.hpp"

#include <cmath>
#include <climits>
#include <algorithm>
#include <vector>

#include <llvm/IR/TypeBuilder.h>

#include "foundations/exceptions.hpp"
#include "foundations/QueryContext.hpp"
#include "foundations/version_management.hpp"

//-----------------------------------------------------------------------------
// NullIndicatorColumn

BitmapTable::BitmapTable() :
        BitmapTable(8) // FIXME once resize() is implemented this will no longer be necessary
{ }

BitmapTable::BitmapTable(size_t columnCountHint)
{
    unsigned bytesPerTuple = static_cast<unsigned>(std::ceil(columnCountHint / 8.0));
    _availableCount = bytesPerTuple*8;
    _data = std::make_unique<Vector>(bytesPerTuple);
}

unsigned BitmapTable::addColumn()
{
    unsigned column = _columnCount;
    _columnCount += 1;
    if (_columnCount > _availableCount) {
        resize(); // allocated one additional byte
    }

    for (size_t tid = 0, limit = _data->size(); tid < limit; ++tid) {
        set(tid, column, false);
    }

    return column;
}

unsigned BitmapTable::cloneColumn(unsigned original)
{
    unsigned column = _columnCount;
    _columnCount += 1;
    if (_columnCount > _availableCount) {
        resize(); // allocated one additional byte
    }

    for (size_t tid = 0, limit = _data->size(); tid < limit; ++tid) {
        set(tid, column, isSet(tid, original));
    }

    return column;
}


void BitmapTable::addRow()
{
    void * row = _data->reserve_back();
    unsigned byteCount = _availableCount/8;
    memset(row, 0, byteCount);
}

void BitmapTable::set(tid_t tid, unsigned column, bool value)
{
    assert(column < _columnCount);

    uint8_t * tuple = static_cast<uint8_t *>(_data->at(tid));

    static_assert(CHAR_BIT == 8, "not supported");
    size_t byte = column >> 3; // column / 8
    unsigned sectionIndex = column & 7; // column % 8

    // calculate section
    uint8_t * sectionPtr = tuple + byte;
    uint8_t section = *sectionPtr;
    section ^= (-value ^ section) & (1 << sectionIndex); // set null indicator bit
    *sectionPtr = section;
}

bool BitmapTable::isSet(tid_t tid, unsigned column) const
{
    assert(column < _columnCount);

    uint8_t * tuple = static_cast<uint8_t *>(_data->at(tid));

    static_assert(CHAR_BIT == 8, "not supported");
    size_t byte = column >> 3; // column / 8
    unsigned sectionIndex = column & 7; // column % 8

    // calculate section
    uint8_t * sectionPtr = tuple + byte;
    uint8_t section = *sectionPtr;
    return static_cast<bool>((section >> sectionIndex) & 1); // test null indicator bit
}

void BitmapTable::resize()
{
    throw NotImplementedException("BitmapTable::resize()");
}

static cg_bool_t isSet_gen(BitmapTable & table, cg_tid_t tid, cg_unsigned_t column)
{
    auto & codeGen = getThreadLocalCodeGen();

    cg_ptr8_t tablePtr = cg_ptr8_t::fromRawPointer(table.data());

    // section index within the row
    cg_size_t sectionIndex( codeGen->CreateLShr(column, 3) );

    cg_size_t bytesPerRow = table.getRowSize();
    cg_size_t offset = tid * bytesPerRow;

    llvm::Value * indicatorIndex = codeGen->CreateAnd(column, cg_unsigned_t(7));
    cg_size_t byteIndex = sectionIndex + offset;

    // calculate section
    cg_ptr8_t sectionPtr( tablePtr + byteIndex.llvmValue );
    cg_u8_t section( codeGen->CreateLoad(cg_u8_t::getType(), sectionPtr) );
    cg_u8_t shifted( codeGen->CreateLShr(section, indicatorIndex) );
    cg_bool_t result( codeGen->CreateTrunc(shifted, cg_bool_t::getType()) );
    return result;
}

cg_bool_t genNullIndicatorLoad(BitmapTable & table, cg_tid_t tid, cg_unsigned_t column)
{
    return isSet_gen(table, tid, column);
}

cg_bool_t isVisibleInBranch(BitmapTable & branchBitmap, cg_tid_t tid, cg_branch_id_t branchId)
{
    return isSet_gen(branchBitmap, tid, branchId);
}

//-----------------------------------------------------------------------------
// Table

Table::Table(Database & db) : _db(db) {

    //Create TID column information
    _tidColumn = std::make_unique<ColumnInformation>();
    _tidColumn->columnName = "tid";
    _tidColumn->type = Sql::getLongIntegerTy(false);

    createBranch(invalid_branch_id);
}

Table::~Table()
{
    for (auto & versionEntry : _version_mgmt_column) {
        destroy_chain(versionEntry.get());
    }
    for (auto & versionEntry : _dangling_version_mgmt_column) {
        destroy_chain(versionEntry.get());
    }
}

void Table::addColumn(const std::string & columnName, Sql::SqlType type)
{
//    _columnNames.push_back(columnName);
    if (_columnsByName.count(columnName) > 0) {
        throw InvalidOperationException("name already in use");
    }

    // the null indicator is not part of the permanent storage layout (for both types)
#ifndef USE_INTERNAL_NULL_INDICATOR
    size_t valueSize = getValueSize( Sql::toNotNullableTy(type) );
#else
    size_t valueSize = getValueSize(type);
#endif

    // set-up column
    auto column = std::make_unique<Vector>(valueSize);
    auto ci = std::make_unique<ColumnInformation>();
    ci->column = column.get();
    ci->columnName = columnName;
    ci->type = type;

    if (type.nullable) {
#ifndef USE_INTERNAL_NULL_INDICATOR
        ci->nullColumnIndex = _nullIndicatorTable.addColumn();
        ci->nullIndicatorType = ColumnInformation::NullIndicatorType::Column;
#else
        ci->nullIndicatorType = ColumnInformation::NullIndicatorType::Embedded;
#endif
    }

//    _columns.emplace(columnName, std::make_pair(std::move(ci), std::move(column)));
    _columnsByName.emplace(columnName, _columns.size());
    _columns.emplace_back(std::move(ci), std::move(column));
}

void Table::addRow(branch_id_t branchId)
{
    for (auto & [ci, vec] : _columns) {
        vec->reserve_back();
    }
    _nullIndicatorTable.addRow();
    _branchBitmap.addRow();
    _branchBitmap.set(_columns.front().second->size() - 1,branchId,1);
}

void Table::removeRowForBranch(tid_t tid, branch_id_t branchId) {
    _branchBitmap.set(tid,branchId,0);
}

void Table::createBranch(branch_id_t parent)
{
    if (parent == invalid_branch_id) {
        _branchBitmap.addColumn();
    } else {
        _branchBitmap.cloneColumn(parent);
    }

}

ci_p_t Table::getCI(const std::string & columnName) const
{
//    return _columns.at(columnName).first.get();
    auto idx = _columnsByName.at(columnName);
    auto & [ci, vec] = _columns[idx];
    return ci.get();
}

const Vector & Table::getColumn(size_t idx) const {
    return *_columns.at(idx).second;
}

const Vector & Table::getColumn(const std::string & columnName) const
{
//    return *_columns.at(columnName).second;
    auto idx = _columnsByName.at(columnName);
    auto & [ci, vec] = _columns[idx];
    return *vec;
}

size_t Table::getColumnCount() const
{
    return _columns.size();
}

bool columnsByNameSort (std::pair<std::string,size_t> &i,std::pair<std::string,size_t> &j) { return (i.second<j.second); }

//const std::vector<std::string> & Table::getColumnNames() const
std::vector<std::string> Table::getColumnNames() const
{
//    return _columnNames;
    using namespace std;
    vector<std::pair<std::string,size_t>> pairs;
    transform(begin(_columnsByName), end(_columnsByName), back_inserter(pairs),
        [](const auto & pair) {
            return pair;
    });
    sort(pairs.begin(),pairs.end(),columnsByNameSort);
    vector<std::string> names;
    transform(begin(pairs), end(pairs), back_inserter(names),
              [](const auto & pair) {
                  return pair.first;
              });
    return names;
}

Database & Table::getDatabase() const {
    return _db;
}

std::vector<Sql::SqlType> Table::getTupleType() const {
    std::vector<Sql::SqlType> tupleType;
    for (const auto & [ci, vec] : _columns) {
        tupleType.push_back(ci->type);
    }
    return tupleType;
}

size_t Table::size() const
{
    if (_columns.empty()) {
        return 0;
    } else {
        return _columns.begin()->second->size();
//        return _columns.begin()->second.second->size();
        return _version_mgmt_column.size();
    }
}

// wrapper functions
static void tableAddRow(Table * table)
{
    table->addRow(0);
}

// generator functions
void genTableAddRowCall(cg_voidptr_t table)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (void *), false>::get(context);
    codeGen.CreateCall(&tableAddRow, funcTy, table.getValue());
}

//-----------------------------------------------------------------------------
// Database

Database::Database() :
    _next_branch_id(master_branch_id)
{
    auto branch = createBranch("master", invalid_branch_id);
    assert(branch == master_branch_id);
}

Table & Database::createTable(const std::string & name) {
    auto [it, ok] = _tables.emplace(name, std::make_unique<Table>(*this));
    assert(ok);
    return *it->second;
}

Table* Database::getTable(const std::string & tableName)
{
    // TODO search case insensitive
    auto it = _tables.find(tableName);
    if (it != _tables.end()) {
        return it->second.get();
    } else {
        return nullptr;
    }
}

branch_id_t Database::getLargestBranchId() const {
    return _next_branch_id - 1;
}

branch_id_t Database::createBranch(const std::string & name, branch_id_t parent) {
    for (auto &[tablename,table] : _tables) {
        table->createBranch(parent);
    }
    branch_id_t branch_id = _next_branch_id++;
    auto branch = std::make_unique<Branch>();
    branch->id = branch_id;
    branch->name = name;
    branch->parent_id = parent;
    _branches.insert({branch_id, std::move(branch)});
    _branchMapping[name] = branch_id;
    return branch_id;
}

void Database::constructBranchLineage(branch_id_t branch, ExecutionContext & dstCtx) {
    assert(branch != invalid_branch_id);

    dstCtx.branch_lineage.clear();
    dstCtx.branch_lineage_bitset.clear();
    dstCtx.branch_lineage_bitset.resize(_next_branch_id);

    branch_id_t current = branch;
    for (;;) {
        dstCtx.branch_lineage_bitset.set(current);
        auto & branch_obj = _branches[current];
        if (branch_obj->parent_id == invalid_branch_id) {
            break;
        }
        dstCtx.branch_lineage.insert({branch_obj->parent_id, current});
        current = branch_obj->parent_id;
    }
}
