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

cg_bool_t isVisibleInBranch(BitmapTable & branchBitmap, cg_tid_t tid, cg_branch_id_t branchId);

//-----------------------------------------------------------------------------
// Table

class Database;
struct VersionEntry;

/// AbstractTable is a base class which provides an interface to lookup columns at runtime
class Table {
public:
    Table(Database & db);

    ~Table();

    void addColumn(const std::string & columnName, Sql::SqlType type);

    void addRow(branch_id_t branchId);

    void removeRow(tid_t tid);

    void removeRowForBranch(tid_t tid, branch_id_t branchId);

    void createBranch(branch_id_t parent);

//    const std::string & getName() const;

    ci_p_t getCI(const std::string & columnName) const;

    std::unique_ptr<ColumnInformation> &getTIDColumnInformation() { return _tidColumn; }

    const Vector & getColumn(size_t idx) const;
    const Vector & getColumn(const std::string & columnName) const;

    /// The count of SQL columns without any null indicator column
    size_t getColumnCount() const;

    std::vector<std::string> getColumnNames() const;

    BitmapTable & getNullIndicatorTable() { return _nullIndicatorTable; }

    BitmapTable & getBranchBitmap() { return _branchBitmap; }

    Database & getDatabase() const;

    std::vector<Sql::SqlType> getTupleType() const;

    size_t size() const;

private:
    Database & _db;

    std::unordered_map<std::string, size_t> _columnsByName; // name -> column index
    std::vector<
        std::pair<std::unique_ptr<ColumnInformation>, std::unique_ptr<Vector>>
        > _columns;

    BitmapTable _nullIndicatorTable;
    BitmapTable _branchBitmap;

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

    bool hasTable(const std::string & tableName) {
        return getTable(tableName) != nullptr;
    }

    branch_id_t getLargestBranchId() const;

private:
    std::unordered_map<std::string, std::unique_ptr<Table>> _tables;
    std::unordered_map<std::string, std::unique_ptr<Index>> _indexes;

public:
    branch_id_t createBranch(const std::string & name, branch_id_t parent);
    void constructBranchLineage(branch_id_t branch, ExecutionContext & dstCtx);

    ident_t getIdent(branch_id_t branch) const { return _branches.at(branch)->ident; }
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
    std::vector<uint64_t> _identNextFree; // first label not yet given to a child
    size_t _relabelCount = 0;

public:

    std::unordered_map<branch_id_t, std::unique_ptr<Branch>> _branches;
    std::unordered_map<std::string, branch_id_t> _branchMapping;
    branch_id_t _next_branch_id;
};
