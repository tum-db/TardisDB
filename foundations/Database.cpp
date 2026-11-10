#include "foundations/Database.hpp"

#include <cmath>
#include <climits>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>
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

    // a row holds the bits of all branches; deletes in other branches may change the same byte meanwhile
    uint8_t * sectionPtr = tuple + byte;
    if (value) {
        __atomic_fetch_or(sectionPtr, static_cast<uint8_t>(1u << sectionIndex), __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_and(sectionPtr, static_cast<uint8_t>(~(1u << sectionIndex)), __ATOMIC_RELAXED);
    }
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
    data->reserve(_data->capacity());
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
    cg_size_t sectionIndex( codeGen->CreateZExt(codeGen->CreateLShr(column, 3), cg_size_t::getType()) );

    cg_size_t bytesPerRow = table.getRowSize();
    cg_size_t offset = tid * bytesPerRow;

    llvm::Value * indicatorIndex = codeGen->CreateTrunc(codeGen->CreateAnd(column, cg_unsigned_t(7)), cg_u8_t::getType());
    cg_size_t byteIndex = sectionIndex + offset;

    // calculate section
    cg_ptr8_t sectionPtr( tablePtr + byteIndex.llvmValue );
    // deletes in other branches may change the byte meanwhile (BitmapTable::set)
    cg_u8_t section( LLVMCompat::makeMonotonic(codeGen->CreateLoad(cg_u8_t::getType(), sectionPtr), 1) );
    cg_u8_t shifted( codeGen->CreateLShr(section, indicatorIndex) );
    cg_bool_t result( codeGen->CreateTrunc(shifted, cg_bool_t::getType()) );
    return result;
}

cg_bool_t genNullIndicatorLoad(BitmapTable & table, cg_tid_t tid, cg_unsigned_t column)
{
    return isSet_gen(table, tid, column);
}

//-----------------------------------------------------------------------------
// BranchMembership

BranchMembership::BranchMembership() : _bitmap(8) { }

void BranchMembership::addBranch(branch_id_t branch, branch_id_t parent)
{
#if LAZY_MEMBERSHIP
    _branches += 1;
#else
    assert(_bitmap.getColumnCount() == branch);
    if (parent == invalid_branch_id) {
        _bitmap.addColumn();
    } else {
        _bitmap.cloneColumn(parent);
    }
#endif
}

void BranchMembership::removeBranch(branch_id_t branch)
{
#if LAZY_MEMBERSHIP
    _branches -= 1;
#else
    for (tid_t tid = 0, rows = size(); tid < rows; ++tid) {
        _bitmap.set(tid, branch, false);
    }
#endif
}

void BranchMembership::removeEntries(const std::vector<bool> & droppedIdents)
{
    for (auto it = _entries.begin(); it != _entries.end(); ) {
        auto & entries = it->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry & entry) {
            return droppedIdents[entry.ident];
        }), entries.end());
        if (entries.empty()) {
            _states[it->first] &= ~diverged;
            it = _entries.erase(it);
        } else {
            ++it;
        }
    }
}

void BranchMembership::addRow(branch_id_t branch)
{
#if LAZY_MEMBERSHIP
    _states.push_back(branch == master_branch_id ? (inRoot | inBranches) : inBranches);
#else
    _bitmap.addRow();
    _bitmap.set(size() - 1, branch, true);
#endif
}

void BranchMembership::reserve(size_t rows)
{
#if LAZY_MEMBERSHIP
    _states.reserve(rows);
#else
    _bitmap.reserve(rows);
#endif
}

size_t BranchMembership::size() const
{
#if LAZY_MEMBERSHIP
    return _states.size();
#else
    return _bitmap.getColumnCount() == 0 ? 0 : _bitmap.size();
#endif
}

bool BranchMembership::containsForLabel(tid_t tid, uint64_t label) const
{
    const uint8_t state = _states[tid];
    if ((state & diverged) != 0) {
        // the entry of the deepest identifier whose label contains the reader's
        const Entry * newest = nullptr;
        uint64_t newestL = 0;
        for (const Entry & entry : _entries.at(tid)) {
            const uint64_t l = _db->_identL[entry.ident];
            if (l <= label && label <= _db->_identR[entry.ident] && (newest == nullptr || l > newestL)) {
                newest = &entry;
                newestL = l;
            }
        }
        if (newest != nullptr) {
            return newest->value;
        }
    }
    return state & inBranches;
}

bool BranchMembership::contains(tid_t tid, branch_id_t branch) const
{
#if LAZY_MEMBERSHIP
    if (branch == master_branch_id) {
        return _states[tid] & inRoot;
    }
    return containsForLabel(tid, _db->_identL[_db->getIdent(branch)]);
#else
    return _bitmap.isSet(tid, branch);
#endif
}

void BranchMembership::set(tid_t tid, branch_id_t branch, bool value)
{
#if LAZY_MEMBERSHIP
    if (contains(tid, branch) == value) {
        return;
    }
    if (branch == master_branch_id) {
        _states[tid] = value ? (_states[tid] | inRoot) : (_states[tid] & ~inRoot);
        if (_branches == 1) {
            // there is no other branch (any more); those forked later start from the new value,
            // and older entries, all of the root, would hide it
            _states[tid] = value ? (_states[tid] | inBranches) : (_states[tid] & ~inBranches);
            _states[tid] &= ~diverged;
            _entries.erase(tid);
            return;
        }
    }
    // the entry of the identifier the branch writes with; branches forked earlier do not see it
    const ident_t ident = _db->getIdent(branch);
    auto & entries = _entries[tid];
    auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry & entry) { return entry.ident == ident; });
    if (it != entries.end()) {
        it->value = value;
    } else {
        entries.push_back({ident, value});
    }
    _states[tid] |= diverged;
#else
    _bitmap.set(tid, branch, value);
#endif
}

size_t BranchMembership::bytes() const
{
#if LAZY_MEMBERSHIP
    // buckets, and per tuple with entries a node with the key, the vector and a link
    size_t bytes = _states.capacity() + _entries.bucket_count() * sizeof(void *) +
                   _entries.size() * (sizeof(tid_t) + sizeof(std::vector<Entry>) + sizeof(void *));
    for (auto & [tid, entries] : _entries) {
        bytes += entries.capacity() * sizeof(Entry);
    }
    return bytes;
#else
    return size() * _bitmap.getRowSize();
#endif
}

size_t BranchMembership::entries() const
{
    size_t count = 0;
    for (auto & [tid, entries] : _entries) {
        count += entries.size();
    }
    return count;
}

#if LAZY_MEMBERSHIP
static size_t branch_membership_contains(void * membership, size_t tid, uint32_t branch)
{
    return static_cast<BranchMembership *>(membership)->contains(tid, branch);
}

static size_t branch_membership_contains_label(void * membership, size_t tid, uint64_t label)
{
    return static_cast<BranchMembership *>(membership)->containsForLabel(tid, label);
}
#endif

cg_bool_t genMembershipTest(BranchMembership & membership, cg_tid_t tid, llvm::Value * branch, branch_id_t constantBranch)
{
#if LAZY_MEMBERSHIP
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    llvm::Type * i8Ty = llvm::Type::getInt8Ty(context);
    llvm::Type * i64Ty = llvm::Type::getInt64Ty(context);

    // the states do not move while the query runs
    cg_ptr8_t states = cg_ptr8_t::fromRawPointer(membership.states());
    llvm::Value * state = codeGen->CreateLoad(i8Ty, codeGen->CreateGEP(i8Ty, states, tid.getValue()));
    if (constantBranch == master_branch_id) {
        return cg_bool_t(codeGen->CreateICmpNE(codeGen->CreateAnd(state, BranchMembership::inRoot), llvm::ConstantInt::get(i8Ty, 0)));
    }
    // a branch other than the root (branch may be the root if not known at compile time)
    llvm::Value * isRoot = codeGen->CreateICmpEQ(branch, llvm::ConstantInt::get(branch->getType(), master_branch_id));
    llvm::Value * bit = codeGen->CreateSelect(isRoot, llvm::ConstantInt::get(i8Ty, BranchMembership::inRoot), llvm::ConstantInt::get(i8Ty, BranchMembership::inBranches));
    llvm::Value * member = codeGen->CreateZExt(codeGen->CreateICmpNE(codeGen->CreateAnd(state, bit), llvm::ConstantInt::get(i8Ty, 0)), i64Ty);
    llvm::Value * isDiverged = codeGen->CreateICmpNE(codeGen->CreateAnd(state, BranchMembership::diverged), llvm::ConstantInt::get(i8Ty, 0));
    IfGen slowPath(codeGen.getCurrentFunctionGen(), codeGen->CreateAnd(isDiverged, codeGen->CreateNot(isRoot)), {{"member", cg_size_t(member)}});
    if (constantBranch != invalid_branch_id) {
        // the label of the reader does not change while the query runs
        const Database & db = *membership.getDatabase();
        llvm::FunctionType * funcTy = llvm::TypeBuilder<size_t (void *, size_t, uint64_t), false>::get(context);
        llvm::Function * func = LLVMCompat::getOrInsertFunction(codeGen.getCurrentModuleGen().getModule(), "branch_membership_contains_label", funcTy);
        codeGen.getCurrentModuleGen().addFunctionMapping(func, (void *) &branch_membership_contains_label);
        slowPath.setVar(0, codeGen->CreateCall(func, {cg_ptr8_t::fromRawPointer(&membership).getValue(), tid.getValue(),
                                                      cg_size_t(db._identL[db.getIdent(constantBranch)]).getValue()}));
    } else {
        llvm::FunctionType * funcTy = llvm::TypeBuilder<size_t (void *, size_t, uint32_t), false>::get(context);
        llvm::Function * func = LLVMCompat::getOrInsertFunction(codeGen.getCurrentModuleGen().getModule(), "branch_membership_contains", funcTy);
        codeGen.getCurrentModuleGen().addFunctionMapping(func, (void *) &branch_membership_contains);
        slowPath.setVar(0, codeGen->CreateCall(func, {cg_ptr8_t::fromRawPointer(&membership).getValue(), tid.getValue(), branch}));
    }
    slowPath.EndIf();
    return cg_bool_t(codeGen->CreateICmpNE(slowPath.getResult(0), llvm::ConstantInt::get(i64Ty, 0)));
#else
    return isSet_gen(membership.bitmap(), tid, cg_unsigned_t(branch));
#endif
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
// ReaderEpochs

unsigned ReaderEpochs::enter()
{
    for (;;) {
        const uint64_t phase = _phase.load();
        const unsigned slot = phase & 1;
        _active[slot].fetch_add(1);
        // synchronize() may have advanced the phase before the increment and found the slot empty
        if (_phase.load() == phase) return slot;
        _active[slot].fetch_sub(1);
    }
}

void ReaderEpochs::leave(unsigned slot)
{
    _active[slot].fetch_sub(1);
}

void ReaderEpochs::synchronize()
{
    // statements that enter from now on use the other slot, which the previous call has emptied
    const unsigned slot = _phase.fetch_add(1) & 1;
    for (unsigned spins = 0; _active[slot].load() != 0; ++spins) {
        if (spins < 64) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}

//-----------------------------------------------------------------------------
// Table

Table::Table(Database & db) : _db(db) {
    _membership.setDatabase(&db);

    //Create TID column information
    _tidColumn = std::make_unique<ColumnInformation>();
    _tidColumn->columnName = "tid";
    _tidColumn->type = Sql::getLongIntegerTy(false);

    createBranch(master_branch_id, invalid_branch_id);
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
    column->reserve(_capacity);
    auto ci = std::make_unique<ColumnInformation>();
    ci->column = column.get();
    ci->columnName = columnName;
    ci->type = type;
    ci->index = _columns.size();
    ci->defaultValue.assign(valueSize, 0);

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
    _baseSchema.push_back(_columns.size());
    for (auto & [branch, schema] : _schemas) {
        schema.push_back(_columns.size());
    }
    _columns.emplace_back(std::move(ci), std::move(column));
}

size_t Table::addBranchColumn(branch_id_t branch, const std::string & columnName, Sql::SqlType type,
                              const std::vector<uint8_t> & defaultValue)
{
    if (findColumn(columnName, branch) != nullptr) {
        throw InvalidOperationException("name already in use");
    }
    const size_t valueSize = getValueSize(Sql::toNotNullableTy(type));
    if (type.nullable || defaultValue.size() != valueSize) {
        throw InvalidOperationException("a column added to a branch needs a default value and no nulls");
    }

    auto column = std::make_unique<Vector>(valueSize);
    column->reserve(_capacity);
    for (size_t row = 0; row < _appended; ++row) {
        std::memcpy(column->reserve_back(), defaultValue.data(), valueSize);
    }
    auto ci = std::make_unique<ColumnInformation>();
    ci->column = column.get();
    ci->columnName = columnName;
    ci->type = type;
    ci->index = _columns.size();
    ci->branchLocal = true;
    ci->addedUnder = _db.getIdent(branch);
    ci->defaultValue = defaultValue;

    std::vector<size_t> schema = getSchema(branch);
    schema.push_back(_columns.size());
    _schemas[branch] = std::move(schema);
    _columns.emplace_back(std::move(ci), std::move(column));
    return _columns.size() - 1;
}

void Table::dropBranchColumn(branch_id_t branch, size_t column)
{
    std::vector<size_t> schema = getSchema(branch);
    schema.erase(std::remove(schema.begin(), schema.end(), column), schema.end());
    _schemas[branch] = std::move(schema);
}

void Table::addToSchema(branch_id_t branch, size_t column)
{
    std::vector<size_t> schema = getSchema(branch);
    if (std::find(schema.begin(), schema.end(), column) == schema.end()) {
        schema.push_back(column);
    }
    _schemas[branch] = std::move(schema);
}

const std::vector<size_t> & Table::getSchema(branch_id_t branch) const
{
    auto it = _schemas.find(branch);
    return (it != _schemas.end()) ? it->second : _baseSchema;
}

std::vector<std::string> Table::getColumnNames(branch_id_t branch) const
{
    std::vector<std::string> names;
    for (size_t column : getSchema(branch)) {
        names.push_back(_columns[column].first->columnName);
    }
    return names;
}

ci_p_t Table::findColumn(const std::string & columnName, branch_id_t branch) const
{
    for (size_t column : getSchema(branch)) {
        if (_columns[column].first->columnName == columnName) {
            return _columns[column].first.get();
        }
    }
    return nullptr;
}

ci_p_t Table::findColumn(const std::string & columnName, const std::vector<branch_id_t> & branches) const
{
    ci_p_t found = nullptr;
    for (branch_id_t branch : branches) {
        ci_p_t ci = findColumn(columnName, branch);
        if (ci == nullptr || (found != nullptr && ci != found)) {
            return nullptr;
        }
        found = ci;
    }
    return found;
}

void Table::removeBranch(branch_id_t branch)
{
    _schemas.erase(branch);
}

void Table::addRow(branch_id_t branchId)
{
    appendRow(branchId);
    publishRows();
}

void Table::appendRow(branch_id_t branchId)
{
    ensureCapacity(_appended + 1);
    for (auto & [ci, vec] : _columns) {
        vec->reserve_back();
    }
#if USE_DATA_VERSIONING
    _nullIndicatorTable.addRow();
    _membership.addRow(branchId);
#endif
    _appended += 1;
}

void Table::publishRows()
{
    _published.store(_appended, std::memory_order_release);
}

void Table::reserveRows(size_t rows)
{
    if (rows <= _capacity) {
        return;
    }
    for (auto & [ci, vec] : _columns) {
        vec->reserve(rows);
    }
    _nullIndicatorTable.reserve(rows);
    _membership.reserve(rows);
    _version_mgmt_column.reserve(rows);
    _head_ident.reserve(rows);
#if COMPILED_LINEAGE
    _head_stamp.reserve(rows);
#endif
    for (auto & [branch, versions] : _versionMaps) {
        versions.reserve(rows);
    }
    _capacity = rows;
}

void Table::ensureCapacity(size_t rows)
{
    if (rows > _capacity) {
        reserveRows(std::max({rows, 2 * _capacity, size_t(1024)}));
    }
}

bool Table::headsKnown()
{
    // a statement may insert meanwhile
    std::lock_guard<std::mutex> lock(_scanLatch);
    return _head_ident.size() == _version_mgmt_column.size();
}

void Table::removeRow(tid_t tid) {
#if !USE_DATA_VERSIONING
    for (auto & [ci, vec] : _columns) {
        vec->remove_at(tid);
    }
    _appended -= 1;
    publishRows();
#endif
}

void Table::removeRowForBranch(tid_t tid, branch_id_t branchId) {
    _membership.set(tid, branchId, false);
}

void Table::createBranch(branch_id_t branch, branch_id_t parent)
{
    _membership.addBranch(branch, parent);
    auto schema = _schemas.find(parent);
    if (schema != _schemas.end()) {
        // copied, since later schema changes of the parent do not reach the child
        std::vector<size_t> copy = schema->second;
        _schemas[branch] = std::move(copy);
    }
}

ci_p_t Table::getCI(const std::string & columnName) const
{
//    return _columns.at(columnName).first.get();
    auto idx = _columnsByName.at(columnName);
    auto & [ci, vec] = _columns[idx];
    return ci.get();
}

ci_p_t Table::getCI(size_t idx) const
{
    return _columns.at(idx).first.get();
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
    // one entry per branch identifier, including those of dropped branches; parents come first
    for (branch_id_t i = 1; i < _next_branch_id; i++) {
        auto branch = _branches.find(i);
        it->second->createBranch(i, branch != _branches.end() ? branch->second->parent_id : invalid_branch_id);
        if (branch == _branches.end()) {
            it->second->getMembership().removeBranch(i);
        }
    }
    it->second->_writes.resize(_identParent.size());
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
    branch_id_t branch_id = _next_branch_id++;
    for (auto &[tablename,table] : _tables) {
        table->createBranch(branch_id, parent);
    }
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
        _identBranch[_branches[parent]->ident] = parent;
        branch->ident = newIdent(old);
    }
    _identBranch[branch->ident] = branch_id;
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
    std::vector<bool> droppedIdents(_identBranch.size(), false);
    for (ident_t ident = 0; ident < _identBranch.size(); ++ident) {
        droppedIdents[ident] = std::find(branches.begin(), branches.end(), _identBranch[ident]) != branches.end();
    }
    for (auto & [name, table] : _tables) {
        for (branch_id_t branch : branches) {
            table->getMembership().removeBranch(branch);
        }
        table->getMembership().removeEntries(droppedIdents);
        for (branch_id_t branch : branches) {
            table->_versionMaps.erase(branch);
            table->_scans.erase(branch);
            table->removeBranch(branch);
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

std::vector<uint32_t> Database::lineageLimits(branch_id_t reader) const {
    std::vector<uint32_t> limits(_next_branch_id, 0);
    limits[reader] = std::numeric_limits<uint32_t>::max();
    for (branch_id_t current = reader; ; ) {
        const branch_id_t parent = _branches.at(current)->parent_id;
        if (parent == invalid_branch_id) break;
        limits[parent] = current;
        current = parent;
    }
    return limits;
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
    _identBranch.push_back(invalid_branch_id); // set by createBranch()
    _identL.push_back(0);
    _identR.push_back(0);
    _identNextFree.push_back(0);
    // writers of different branches record their writes at the same time, each in the list of its own
    // identifier: the lists exist before any write
    for (auto & [name, table] : _tables) {
        if (table->_writes.size() <= id) table->_writes.resize(id + 1);
    }

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
