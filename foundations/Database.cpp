#include "foundations/Database.hpp"

#include <cmath>
#include <climits>
#include <cstring>
#include <algorithm>
#include <vector>

#include "codegen/LLVMCompat.hpp"

#include "foundations/exceptions.hpp"
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
    // double the bytes per row and copy the rows; compiled code embeds the data pointer
    // and the row size, but every statement is compiled anew
    const size_t oldBytes = _data->getElementSize();
    const size_t newBytes = 2 * oldBytes;
    const size_t rows = _data->size();
    auto data = std::make_unique<Vector>(newBytes);
    for (size_t tid = 0; tid < rows; ++tid) {
        auto * row = static_cast<uint8_t *>(data->reserve_back());
        std::memcpy(row, _data->at(tid), oldBytes);
        std::memset(row + oldBytes, 0, newBytes - oldBytes);
    }
    _data = std::move(data);
    _availableCount = newBytes * 8;
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
// BranchIndex

void BranchIndex::add(uint64_t hash, tid_t tid)
{
    auto & tids = _entries[hash];
    if (std::find(tids.begin(), tids.end(), tid) == tids.end()) {
        tids.push_back(tid);
        _size += 1;
    }
}

const std::vector<tid_t> * BranchIndex::lookup(uint64_t hash) const
{
    auto it = _entries.find(hash);
    return (it != _entries.end()) ? &it->second : nullptr;
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
#if USE_DATA_VERSIONING
    _nullIndicatorTable.addRow();
    _branchBitmap.addRow();
    _branchBitmap.set(_columns.front().second->size() - 1,branchId,1);
#endif
}

void Table::removeRow(tid_t tid) {
#if !USE_DATA_VERSIONING
    for (auto & [ci, vec] : _columns) {
        vec->remove_at(tid);
    }
#endif
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

BranchIndex * Table::getIndex(size_t column, branch_id_t branch) const
{
    for (auto & index : _indexes) {
        if (index->column == column && index->branch == branch) {
            return index.get();
        }
    }
    return nullptr;
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
    // the root branch is also known under the name of today's version control systems
    _branchMapping["main"] = master_branch_id;
}

Table & Database::createTable(const std::string & name) {
    auto [it, ok] = _tables.emplace(name, std::make_unique<Table>(*this));
    assert(ok);
    // one bitmap column per branch identifier, including those of dropped branches
    for (branch_id_t i = 1; i < _next_branch_id; i++) {
        it->second->createBranch(invalid_branch_id);
    }
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
    if (parent == invalid_branch_id) {
        branch->ident = newIdent(invalid_ident);
    } else {
        // the parent continues under a new identifier, so that its later writes are not inherited
        ident_t old = _branches[parent]->ident;
        _branches[parent]->ident = newIdent(old);
        branch->ident = newIdent(old);
    }
    _branches.insert({branch_id, std::move(branch)});
    _branchMapping[name] = branch_id;
    return branch_id;
}

std::vector<branch_id_t> Database::getSubtree(branch_id_t branch) const {
    // children have larger identifiers than their parents
    std::vector<branch_id_t> subtree = {branch};
    std::set<branch_id_t> members = {branch};
    std::vector<branch_id_t> ids;
    for (auto & [id, b] : _branches) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    for (branch_id_t id : ids) {
        if (members.count(_branches.at(id)->parent_id) > 0) {
            members.insert(id);
            subtree.push_back(id);
        }
    }
    return subtree;
}

void Database::removeBranches(const std::vector<branch_id_t> & branches) {
    for (auto & [name, table] : _tables) {
        auto & bitmap = table->getBranchBitmap();
        for (branch_id_t branch : branches) {
            for (tid_t tid = 0, size = table->size(); tid < size; ++tid) {
                bitmap.set(tid, branch, false);
            }
        }
        auto & indexes = table->_indexes;
        indexes.erase(std::remove_if(indexes.begin(), indexes.end(), [&](auto & index) {
            return std::find(branches.begin(), branches.end(), index->branch) != branches.end();
        }), indexes.end());
    }
    for (branch_id_t branch : branches) {
        for (auto it = _branchMapping.begin(); it != _branchMapping.end(); ) {
            it = (it->second == branch) ? _branchMapping.erase(it) : std::next(it);
        }
        _branches.erase(branch);
    }
}

void Database::constructBranchLineage(branch_id_t branch, ExecutionContext & dstCtx) {
    assert(branch != invalid_branch_id);

    if (dstCtx.branch_lineages.find(branch) == dstCtx.branch_lineages.end()) {
        dstCtx.branch_lineages[branch] = std::unordered_map<branch_id_t, branch_id_t>();
    }
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
        dstCtx.branch_lineages[branch].insert({branch_obj->parent_id, current});
        dstCtx.branch_lineage.insert({branch_obj->parent_id, current});
        current = branch_obj->parent_id;
    }
    dstCtx.lineage_branch = branch;

    dstCtx.branch_labels[branch] = _identL[_branches[branch]->ident];
    dstCtx.reader_label = dstCtx.branch_labels[branch];
    dstCtx.ident_l = _identL.data();
    dstCtx.ident_r = _identR.data();
}

ident_t Database::newIdent(ident_t parent) {
    ident_t id = static_cast<ident_t>(_identParent.size());
    _identParent.push_back(parent);
    _identL.push_back(0);
    _identR.push_back(0);
    _identNextFree.push_back(0);

    if (parent == invalid_ident) {
        _identL[id] = 0;
        _identR[id] = (uint64_t(1) << 62) - 1;
        _identNextFree[id] = 1;
        return id;
    }
    // a new child takes half of the labels its parent has left
    const uint64_t left = _identR[parent] >= _identNextFree[parent] ? _identR[parent] - _identNextFree[parent] + 1 : 0;
    if (left < 4) {
        relabel();
        return id;
    }
    _identL[id] = _identNextFree[parent];
    _identR[id] = _identL[id] + left / 2 - 1;
    _identNextFree[id] = _identL[id] + 1;
    _identNextFree[parent] = _identR[id] + 1;
    return id;
}

void Database::relabel() {
    // every identifier receives a range proportional to its subtree, leaving gap - 1 free labels
    // in each; identifiers are created after their parents, so a reverse pass sums the subtrees
    const size_t count = _identParent.size();
    std::vector<uint64_t> size(count, 1);
    std::vector<std::vector<ident_t>> children(count);
    for (size_t i = count; i-- > 0; ) {
        if (_identParent[i] != invalid_ident) {
            size[_identParent[i]] += size[i];
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (_identParent[i] != invalid_ident) {
            children[_identParent[i]].push_back(static_cast<ident_t>(i));
        }
    }
    const uint64_t gap = std::min<uint64_t>(uint64_t(1) << 20, ((uint64_t(1) << 62) - 1) / size[0]);
    std::vector<std::pair<ident_t, uint64_t>> stack = {{0, 0}};
    while (!stack.empty()) {
        auto [id, start] = stack.back();
        stack.pop_back();
        _identL[id] = start;
        _identR[id] = start + size[id] * gap - 1;
        uint64_t next = start + 1;
        for (ident_t child : children[id]) {
            stack.push_back({child, next});
            next += size[child] * gap;
        }
        _identNextFree[id] = next;
    }
    ++_relabelCount;
}
