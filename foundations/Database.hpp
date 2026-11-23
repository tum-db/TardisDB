#pragma once

#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <limits>
#include <atomic>
#include <mutex>
#include <shared_mutex>

#include "utils/shared_latch.hpp"

#include "sql/SqlType.hpp"
#include "Vector.hpp"

//#include "foundations/version_management.hpp"

using branch_id_t = uint32_t;
using cg_branch_id_t = TypeWrappers::UInt32;
constexpr branch_id_t master_branch_id = 0;
constexpr branch_id_t invalid_branch_id = std::numeric_limits<branch_id_t>::max();

/// identifier under which a branch writes; a fork gives the parent and the child a new one
using ident_t = uint32_t;
constexpr ident_t invalid_ident = std::numeric_limits<ident_t>::max();
/// the writer and the time of a version in one word, for the compiled lineage test (COMPILED_LINEAGE)
inline uint64_t lineage_stamp(uint32_t branch, uint32_t creation_ts) { return (uint64_t(branch) << 32) | creation_ts; }
constexpr uint64_t invalid_stamp = std::numeric_limits<uint64_t>::max();

using tid_t = size_t;
using cg_tid_t = cg_size_t;
constexpr tid_t invalid_tid = std::numeric_limits<tid_t>::max();

//-----------------------------------------------------------------------------
// ColumnInformation

/// ColumnInformation contains the column pointer and a description about the column type
struct ColumnInformation {
    Vector * column;
    std::string columnName;
    Sql::SqlType type;

    size_t index = 0; // position in the tuples of the table (Table::getTupleType())
    /// added by ALTER TABLE: only the branches whose schema lists it have it, and versions written
    /// before it was added hold no value for it
    bool branchLocal = false;
    /// the identifier under which the branch added it, or invalid_ident; MERGE takes over the columns
    /// added after the source and the target diverged
    ident_t addedUnder = invalid_ident;
    /// the stored value a version without a value for the column reads
    std::vector<uint8_t> defaultValue;
};

using ci_p_t = const ColumnInformation *;

//-----------------------------------------------------------------------------
// BitmapTable

/// Rows of bits, one column per branch (the eager membership)
class BitmapTable {
public:
    BitmapTable(size_t columnCountHint);

    unsigned addColumn();

    unsigned cloneColumn(unsigned original);

    unsigned getColumnCount() const { return _columnCount; }

    void addRow();

    /// Makes room for the rows, so that adding up to that many rows does not move them
    void reserve(size_t rows) { _data->reserve(rows); }

    /// \returns Bytes per row
    size_t getRowSize() const { return _data->getElementSize(); }

    size_t size() const { return _data->size(); }

    void set(tid_t tid, unsigned column, bool value);

    bool isSet(tid_t tid, unsigned column) const;

    void * data() const { return _data->front(); }

private:
    void resize();

    unsigned _availableCount = 0;
    unsigned _columnCount = 0;
    std::unique_ptr<Vector> _data;
};

//-----------------------------------------------------------------------------
// BranchMembership

class Database;

/// Which tuples of a table belong to which branch; the version chain decides which version a
/// member reads. Two implementations, chosen by the CMake option LAZY_MEMBERSHIP (the layout of
/// the class does not depend on it):
///
/// Eager (TardisDB): one bitmap column per branch; a fork copies the column of its parent.
///
/// Lazy: a fork copies nothing. A change of membership (a delete, or an insert by a merge) is an
/// entry (identifier, value) of the tuple, under the identifier the branch writes with. A fork
/// gives the parent and the child new identifiers, so later changes of the parent do not reach the
/// child, as for versions. A branch sees the entries of the ancestors of its identifier, and the
/// newest of them, the one of the deepest identifier, decides; with interval labels, that is the
/// containing label with the largest left end, independent of the depth. Inserts need no entry:
/// the versions decide who sees an inserted tuple, so a branch contains the tuples others inserted,
/// and scans skip members without a visible version. One byte per tuple holds the membership of the
/// root and of the other branches without entries, and whether the tuple has entries, so that the
/// compiled scan looks up entries only for the few tuples that were deleted somewhere.
class BranchMembership {
public:
    static constexpr uint8_t inRoot = 1;     // the tuple belongs to the root branch
    static constexpr uint8_t diverged = 2;   // the tuple has entries
    static constexpr uint8_t inBranches = 4; // the tuple belongs to the other branches, unless entries say otherwise

    BranchMembership();

    void setDatabase(const Database * db) { _db = db; }
    const Database * getDatabase() const { return _db; }

    /// Adds a branch with the identifier that follows all others
    void addBranch(branch_id_t branch, branch_id_t parent);

    void removeBranch(branch_id_t branch);

    /// Removes the entries of the identifiers of dropped branches
    void removeEntries(const std::unordered_set<ident_t> & droppedIdents);

    /// Moves the entry of the tuple under \p from to \p to, its parent identifier (see fold_ident())
    void retagEntries(tid_t tid, ident_t from, ident_t to);

    /// Adds a tuple, which the branch has inserted
    void addRow(branch_id_t branch);

    /// Makes room for the rows, so that adding up to that many rows does not move the membership
    void reserve(size_t rows);

    bool contains(tid_t tid, branch_id_t branch) const;

    /// As contains() for a branch other than the root with the given label
    bool containsForLabel(tid_t tid, uint64_t label) const;

    void set(tid_t tid, branch_id_t branch, bool value);

    size_t size() const;

    /// \returns The memory held by the membership structures
    size_t bytes() const;

    /// \returns The number of entries (lazy)
    size_t entries() const;

    /// Lazy: per tuple, inRoot | diverged | inBranches
    const uint8_t * states() const { return _states.data(); }

    /// Eager: one column per branch
    BitmapTable & bitmap() { return _bitmap; }

private:
    struct Entry {
        ident_t ident;
        bool value;
    };

    const Database * _db = nullptr;
    BitmapTable _bitmap;
    std::vector<uint8_t> _states;
    std::unordered_map<tid_t, std::vector<Entry>> _entries;
    size_t _branches = 0;
};

/// \returns Whether the branch contains the tuple; branch is an i32 value, constantBranch its value
/// if known at compile time
cg_bool_t genMembershipTest(BranchMembership & membership, cg_tid_t tid, llvm::Value * branch,
                            branch_id_t constantBranch = invalid_branch_id);

//-----------------------------------------------------------------------------
// Table

class Database;
struct VersionEntry;
struct InPlaceWrites;

/// Grace periods for the in-place writes of main (see InPlaceWrites in version_management.hpp): a
/// statement that may read the tuples of a table in place enters its epochs once it holds its
/// latches and leaves them at its end; synchronize() returns once every statement that entered
/// before it was called has left. A statement that has entered never waits for another one.
class ReaderEpochs {
public:
    /// \returns The slot to pass to leave()
    unsigned enter();

    void leave(unsigned slot);

    /// Waits for the statements that have entered; one thread at a time
    void synchronize();

private:
    std::atomic<uint64_t> _phase{0};
    std::atomic<size_t> _active[2] = {{0}, {0}};
};

/// A hash index on one column of a table, local to one branch: CREATE INDEX ON t (c) VERSION b.
/// It covers the tuples of the branch and is maintained by the writes of the branch only, since
/// writes in other branches are not visible in it. Entries are candidates: the scan still tests
/// visibility and the predicate, so entries of overwritten or deleted versions may remain.
class BranchIndex {
public:
    BranchIndex(std::string name, size_t column, branch_id_t branch) :
            name(std::move(name)), column(column), branch(branch)
    { }

    /// Adds the tuple under the hash of its key, unless it is there already
    void add(uint64_t hash, tid_t tid);

    /// \returns The candidates for the key with the given hash, or nullptr
    const std::vector<tid_t> * lookup(uint64_t hash) const;

    size_t size() const { return _size; }

    const std::string name;
    const size_t column;
    const branch_id_t branch;

private:
    std::unordered_map<uint64_t, std::vector<tid_t>> _entries;
    size_t _size = 0;
};

/// AbstractTable is a base class which provides an interface to lookup columns at runtime
class Table {
public:
    Table(Database & db);

    ~Table();

    /// Adds a column of every branch (CREATE TABLE)
    void addColumn(const std::string & columnName, Sql::SqlType type);

    /// Adds a column to the schema of the branch only (ALTER TABLE ... ADD COLUMN): the rows hold the
    /// default in place, and versions written before have no value for it, so they read the default.
    /// No other statement may use the table. \returns The position of the column
    size_t addBranchColumn(branch_id_t branch, const std::string & columnName, Sql::SqlType type,
                           const std::vector<uint8_t> & defaultValue);

    /// Removes the column from the schema of the branch (ALTER TABLE ... DROP COLUMN); its storage
    /// stays, since other branches may have it
    void dropBranchColumn(branch_id_t branch, size_t column);

    /// Adds the column to the schema of the branch (MERGE takes over the columns of the source)
    void addToSchema(branch_id_t branch, size_t column);

    /// \returns The positions of the columns the branch has, in the order of SELECT *. A branch inherits
    /// the schema of its parent at the fork; branches without a schema change share the columns of
    /// CREATE TABLE
    const std::vector<size_t> & getSchema(branch_id_t branch) const;

    /// \returns The names of the columns of the branch, in the order of getSchema()
    std::vector<std::string> getColumnNames(branch_id_t branch) const;

    /// The columns of PRIMARY KEY, in their order; only recorded (for catalogs), not enforced
    const std::vector<std::string> & getPrimaryKey() const { return _primaryKey; }
    void setPrimaryKey(std::vector<std::string> columns) { _primaryKey = std::move(columns); }

    /// \returns The column of the branch with the name, or nullptr
    ci_p_t findColumn(const std::string & columnName, branch_id_t branch) const;

    /// \returns The column that all the branches have under the name, or nullptr; branches may have
    /// different columns of the same name
    ci_p_t findColumn(const std::string & columnName, const std::vector<branch_id_t> & branches) const;

    /// Forgets the schema of a dropped branch
    void removeBranch(branch_id_t branch);

    /// The branch-local columns added under \p from count as added under \p to (see fold_ident())
    void retagColumns(ident_t from, ident_t to);

    /// Adds a row and counts it in size()
    void addRow(branch_id_t branchId);

    /// Adds a row without counting it in size(): statements that read the table meanwhile do not see it
    void appendRow(branch_id_t branchId);

    /// Counts the rows appended so far in size(), once they are complete
    void publishRows();

    /// Makes room for the rows in every array with an element per tuple (see _capacity); moves them,
    /// so no other statement may use the table
    void reserveRows(size_t rows);

    /// Makes room for the rows if there is not enough, see reserveRows()
    void ensureCapacity(size_t rows);

    /// \returns The rows the arrays with an element per tuple hold without moving
    size_t capacity() const { return _capacity; }

    /// \returns Whether every tuple has a head identifier (rows loaded otherwise have none)
    bool headsKnown();

    void removeRowForBranch(tid_t tid, branch_id_t branchId);

    void createBranch(branch_id_t branch, branch_id_t parent);

//    const std::string & getName() const;

    /// \returns A column of CREATE TABLE
    ci_p_t getCI(const std::string & columnName) const;

    /// \returns The column at the position
    ci_p_t getCI(size_t idx) const;

    std::unique_ptr<ColumnInformation> &getTIDColumnInformation() { return _tidColumn; }

    const Vector & getColumn(size_t idx) const;
    const Vector & getColumn(const std::string & columnName) const;

    /// The count of SQL columns of all branches
    size_t getColumnCount() const;

    /// \returns The names of the columns of CREATE TABLE
    std::vector<std::string> getColumnNames() const;

    BranchMembership & getMembership() { return _membership; }

    Database & getDatabase() const;

    std::vector<Sql::SqlType> getTupleType() const;

    /// \returns The rows published so far; statements compile their scans for this many rows
    size_t size() const { return _published.load(std::memory_order_acquire); }

    /// \returns The index of the branch on the column, or nullptr
    BranchIndex * getIndex(size_t column, branch_id_t branch) const;

    std::vector<std::unique_ptr<BranchIndex>> _indexes;

private:
    Database & _db;

    std::vector<std::string> _primaryKey;
    std::unordered_map<std::string, size_t> _columnsByName; // name -> column index, for the columns of CREATE TABLE
    std::vector<size_t> _baseSchema; // the columns of CREATE TABLE
    std::unordered_map<branch_id_t, std::vector<size_t>> _schemas; // per branch that differs from _baseSchema
    std::vector<
        std::pair<std::unique_ptr<ColumnInformation>, std::unique_ptr<Vector>>
        > _columns;

    BranchMembership _membership;

    std::unique_ptr<ColumnInformation> _tidColumn;

    size_t _appended = 0; // the rows appended, by the statement that inserts
    std::atomic<size_t> _published{0};
    /// the columns, the membership, the version entries, the head identifiers and the
    /// version maps hold this many rows without moving, so that a statement can insert beside others
    /// that read them through pointers compiled into their code
    size_t _capacity = 0;

public:
    std::vector<std::unique_ptr<VersionEntry>> _version_mgmt_column;
    /// per tuple: the identifier of the in-place version if it heads the chain, otherwise invalid_ident;
    /// lets the compiled scan decide visibility without dereferencing the version entry
    std::vector<ident_t> _head_ident;
#if COMPILED_LINEAGE
    /// as _head_ident, the lineage_stamp() of the in-place version if it heads the chain, otherwise invalid_stamp
    std::vector<uint64_t> _head_stamp;
#endif
    /// per identifier: the tuples written under it (versions, deletes, inserts); two branches can
    /// differ only in tuples written under an identifier that is an ancestor of one of them only
    std::vector<std::vector<tid_t>> _writes;
    /// per identifier, the tuples whose last write under it was a delete: whether two branches had a
    /// tuple when they split (MERGE); kept until the branch of the identifier is dropped
    std::vector<std::unordered_set<tid_t>> _deleted;
    /// per branch with a version map, per tuple the version the branch reads: 0 in place, 1 none (the tuple
    /// belongs to the branch, but it sees no version), otherwise the tuple data of the version.
    /// Only the writes of the branch change what it reads; see build_version_map()
    std::unordered_map<branch_id_t, std::vector<uintptr_t>> _versionMaps;
    std::unordered_map<branch_id_t, size_t> _scans; // per branch, the scans compiled so far
    std::vector<std::unique_ptr<VersionEntry>> _dangling_version_mgmt_column;

    /// held shared by the statements that read the table and exclusive by those that write it; with
    /// QueryCompiler::Latching::Branch, updates and eager deletes hold it shared and latch their branch
    SharedLatch _latch;
    /// guards _scans, the map _versionMaps and _inPlaceWrites while scans of several branches are compiled at once
    std::mutex _scanLatch;
    /// the statements that may read tuples in place while main writes the table
    ReaderEpochs _readers;
    /// the in-place writes of the statement of main that writes the table beside readers of other
    /// branches, completed at the end of the statement; nullptr if there is none
    InPlaceWrites * _inPlaceWrites = nullptr;
    /// held by the statement that inserts into the table, from before its table latch to its end
    std::mutex _insertLatch;
};

//-----------------------------------------------------------------------------
// Index
class Index {
public:
    virtual ~Index() { }

private:
    std::vector<ci_p_t> _key;
};

class ARTIndex : public Index {

};

class BTreeIndex : public Index {

};


//-----------------------------------------------------------------------------
// Branch

struct Branch {
    branch_id_t id;
    branch_id_t parent_id;
    std::string name;
    ident_t ident; // current identifier
    /// the private branch of a transaction (QueryCompiler::beginTransaction): not listed, not selected
    /// by VERSION ALL, DESCENDANTS or LEAVES, and its parent cannot be dropped while it exists
    bool transaction = false;
    /// held shared by the statements that read the branch and exclusive by those that update or delete
    /// in it while they hold the tables shared (see QueryCompiler::Latching::Branch)
    SharedLatch latch;
};

//-----------------------------------------------------------------------------
// Database

struct ExecutionContext;
class Database;

/// Tables that only the current thread sees, under names that shadow those of the catalogue, while the
/// object exists: the results of the common table expressions of WITH and the row of a SELECT without
/// FROM. Only statements of the thread read them, in the master branch. The statement that creates them
/// holds the catalog latch shared, so that no branch is created or dropped meanwhile, which these
/// tables, unknown to the catalogue, would miss.
class TemporaryTables {
public:
    explicit TemporaryTables(Database & db);
    ~TemporaryTables();
    TemporaryTables(const TemporaryTables &) = delete;
    TemporaryTables & operator=(const TemporaryTables &) = delete;

    /// \returns A new table with the columns, not yet named
    Table & create(const std::vector<std::pair<std::string, Sql::SqlType>> & columns);
    /// Names the table (nullptr: the name is no longer temporary)
    void bind(const std::string & name, Table * table);
    /// Frees a table that has no name and that no statement reads any more
    void release(Table * table);
    /// \returns The temporary table of the name on the current thread, or nullptr
    static Table * find(const Database & db, const std::string & name);

private:
    Database & _db;
    TemporaryTables * _previous;
    std::unordered_map<std::string, Table *> _names;
    std::vector<std::unique_ptr<Table>> _tables;
};

class Database {
public:
    Database();

    Table & createTable(const std::string & name);

    /// \returns A table outside the catalogue with the branches of the catalogue (see TemporaryTables)
    std::unique_ptr<Table> createTemporaryTable();

    /// \returns The table of the name: a temporary table of the current thread or one of the catalogue
    Table * getTable(const std::string & tableName);

    const std::unordered_map<std::string, std::unique_ptr<Table>> & getTables() const { return _tables; }

    bool hasTable(const std::string & tableName) {
        return getTable(tableName) != nullptr;
    }

    branch_id_t getLargestBranchId() const;

private:
    std::unordered_map<std::string, std::unique_ptr<Table>> _tables;
    std::unordered_map<std::string, std::unique_ptr<Index>> _indexes;

public:
    branch_id_t createBranch(const std::string & name, branch_id_t parent);

    /// \returns The branch and its descendants, children after their parents
    std::vector<branch_id_t> getSubtree(branch_id_t branch) const;

    /// Removes the branches from the catalogue and their bits from the bitmaps, and frees their
    /// identifiers \p idents, which no version names any more, for reuse; branch ids are not reused.
    /// Versions are collected by drop_branches() in version_management.hpp.
    void removeBranches(const std::vector<branch_id_t> & branches, const std::vector<ident_t> & idents);
    void constructBranchLineage(branch_id_t branch, ExecutionContext & dstCtx);
    /// The lineage of a reader as a dense array over all branches: a version written by branch b at
    /// creation_ts is visible to the reader iff creation_ts < limits[b]. limits[reader] is the
    /// maximum, limits[a] of an ancestor a the branch id of its child on the path (forked after
    /// everything a wrote before), and 0 for all other branches (COMPILED_LINEAGE).
    std::vector<uint32_t> lineageLimits(branch_id_t reader) const;

    ident_t getIdent(branch_id_t branch) const { return _branches.at(branch)->ident; }
    /// \returns The branch that writes or wrote under the identifier
    branch_id_t getIdentBranch(ident_t ident) const { return _identBranch[ident]; }
    ident_t getIdentParent(ident_t ident) const { return _identParent[ident]; }
    const std::vector<ident_t> & getIdentChildren(ident_t ident) const { return _identChildren[ident]; }
    /// \returns Whether \p p is \p q or one of its ancestors
    bool isAncestorIdent(ident_t p, ident_t q) const { return _identL[p] <= _identL[q] && _identL[q] <= _identR[p]; }
    /// \returns The identifiers of the tree from the root, parents before their children. Identifiers
    /// are reused, so a child's may be smaller than its parent's.
    std::vector<ident_t> identPreorder() const;
    /// \returns The size of the identifier arrays, freed identifiers included
    size_t getIdentCount() const { return _identParent.size(); }
    size_t getRelabelCount() const { return _relabelCount; }

    /// \returns An identifier outside of the tree that no reader sees, for the in-place versions of
    /// tuples that a dropped branch inserted (see drop_branches())
    ident_t deadIdent();

    /// Folds \p child, the only child left of its parent, into the parent: the children and the
    /// branch of \p child move to the parent, and \p child is freed. Versions are retagged by
    /// fold_ident() in version_management.cpp.
    void foldIdent(ident_t child);

    /// Interval labels of the identifier tree: identifier p is an ancestor of q (or q itself)
    /// iff _identL[p] <= _identL[q] <= _identR[p]. Versions store identifiers, not labels,
    /// so relabelling changes only these arrays.
    std::vector<uint64_t> _identL;
    std::vector<uint64_t> _identR;

private:
    ident_t newIdent(ident_t parent);
    void relabel();

    void freeIdent(ident_t ident);

    std::vector<ident_t> _identParent;
    std::vector<std::vector<ident_t>> _identChildren;
    std::vector<branch_id_t> _identBranch;
    std::vector<uint64_t> _identNextFree; // first label not yet given to a child
    std::vector<ident_t> _freeIdents;     // reused by newIdent()
    ident_t _deadIdent = invalid_ident;
    size_t _relabelCount = 0;

public:

    std::unordered_map<branch_id_t, std::unique_ptr<Branch>> _branches;
    std::unordered_map<std::string, branch_id_t> _branchMapping;
    branch_id_t _next_branch_id;

    /// per (source, target) of a merge and per tuple: the bytes of the source version merged last,
    /// which is the base of the next merge of the pair
    std::map<std::pair<branch_id_t, branch_id_t>,
             std::unordered_map<const Table *, std::unordered_map<tid_t, std::vector<uint8_t>>>> _mergeBases;

    /// held exclusive by the statements that change tables or branches (CREATE, DROP, MERGE, COPY),
    /// shared by all others, which then latch the tables they access
    SharedLatch _catalogLatch;
};
