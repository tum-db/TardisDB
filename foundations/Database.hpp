#pragma once

#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <limits>

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

    enum class NullIndicatorType { Embedded, Column } nullIndicatorType;
    unsigned nullColumnIndex;
};

using ci_p_t = const ColumnInformation *;

//-----------------------------------------------------------------------------
// NullIndicatorColumn

class BitmapTable {
public:
    BitmapTable();

    BitmapTable(size_t columnCountHint);

    unsigned addColumn();

    unsigned cloneColumn(unsigned original);

    void removeColumn(unsigned column);

    unsigned getColumnCount() const { return _columnCount; }

    void addRow();

    void removeRow();

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

/// \param columnAddr The raw address obtained through NullIndicatorColumn::data()
cg_bool_t genNullIndicatorLoad(BitmapTable & table, cg_tid_t tid, cg_unsigned_t column);

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
    void removeEntries(const std::vector<bool> & droppedIdents);

    /// Adds a tuple, which the branch has inserted
    void addRow(branch_id_t branch);

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

    void addColumn(const std::string & columnName, Sql::SqlType type);

    void addRow(branch_id_t branchId);

    void removeRow(tid_t tid);

    void removeRowForBranch(tid_t tid, branch_id_t branchId);

    void createBranch(branch_id_t branch, branch_id_t parent);

//    const std::string & getName() const;

    ci_p_t getCI(const std::string & columnName) const;

    std::unique_ptr<ColumnInformation> &getTIDColumnInformation() { return _tidColumn; }

    const Vector & getColumn(size_t idx) const;
    const Vector & getColumn(const std::string & columnName) const;

    /// The count of SQL columns without any null indicator column
    size_t getColumnCount() const;

    std::vector<std::string> getColumnNames() const;

    BitmapTable & getNullIndicatorTable() { return _nullIndicatorTable; }

    BranchMembership & getMembership() { return _membership; }

    Database & getDatabase() const;

    std::vector<Sql::SqlType> getTupleType() const;

    size_t size() const;

    /// \returns The index of the branch on the column, or nullptr
    BranchIndex * getIndex(size_t column, branch_id_t branch) const;

    std::vector<std::unique_ptr<BranchIndex>> _indexes;

private:
    Database & _db;

    std::unordered_map<std::string, size_t> _columnsByName; // name -> column index
    std::vector<
        std::pair<std::unique_ptr<ColumnInformation>, std::unique_ptr<Vector>>
        > _columns;

    BitmapTable _nullIndicatorTable;
    BranchMembership _membership;

    std::unique_ptr<ColumnInformation> _tidColumn;

public:
    std::vector<std::unique_ptr<VersionEntry>> _version_mgmt_column;
    /// per tuple: the identifier of the in-place version if it heads the chain, otherwise invalid_ident;
    /// lets the compiled scan decide visibility without dereferencing the version entry
    std::vector<ident_t> _head_ident;
    std::vector<std::unique_ptr<VersionEntry>> _dangling_version_mgmt_column;
};

void genTableAddRowCall(cg_voidptr_t table);

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
};

//-----------------------------------------------------------------------------
// Database

struct ExecutionContext;

class Database {
public:
    Database();

    Table & createTable(const std::string & name);

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

    /// Removes the branches from the catalogue and their bits from the bitmaps; their identifiers
    /// are not reused. Versions are collected by drop_branches() in version_management.hpp.
    void removeBranches(const std::vector<branch_id_t> & branches);
    void constructBranchLineage(branch_id_t branch, ExecutionContext & dstCtx);

    ident_t getIdent(branch_id_t branch) const { return _branches.at(branch)->ident; }
    /// \returns The branch that writes or wrote under the identifier
    branch_id_t getIdentBranch(ident_t ident) const { return _identBranch[ident]; }
    size_t getIdentCount() const { return _identParent.size(); }
    size_t getRelabelCount() const { return _relabelCount; }

    /// Interval labels of the identifier tree: identifier p is an ancestor of q (or q itself)
    /// iff _identL[p] <= _identL[q] <= _identR[p]. Versions store identifiers, not labels,
    /// so relabelling changes only these arrays.
    std::vector<uint64_t> _identL;
    std::vector<uint64_t> _identR;

private:
    ident_t newIdent(ident_t parent);
    void relabel();

    std::vector<ident_t> _identParent;
    std::vector<branch_id_t> _identBranch;
    std::vector<uint64_t> _identNextFree; // first label not yet given to a child
    size_t _relabelCount = 0;

public:

    std::unordered_map<branch_id_t, std::unique_ptr<Branch>> _branches;
    std::unordered_map<std::string, branch_id_t> _branchMapping;
    branch_id_t _next_branch_id;

    /// per (source, target) of a merge and per tuple: the bytes of the source version merged last,
    /// which is the base of the next merge of the pair
    std::map<std::pair<branch_id_t, branch_id_t>,
             std::unordered_map<const Table *, std::unordered_map<tid_t, std::vector<uint8_t>>>> _mergeBases;
};
