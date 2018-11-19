#pragma once

#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <limits>

#include "sql/SqlType.hpp"
#include "Vector.hpp"

using branch_id_t = int32_t;
using cg_branch_id_t = TypeWrappers::UInt32;
constexpr branch_id_t master_branch_id = 0;

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

/// AbstractTable is a base class which provides an interface to lookup columns at runtime
class Table {
public:
    Table();
    ~Table() = default;

    void addColumn(const std::string & columnName, Sql::SqlType type);

    void addRow();

    void createBranch(const std::string & name);

//    const std::string & getName() const;

    ci_p_t getCI(const std::string & columnName) const;

    const Vector & getColumn(const std::string & columnName) const;

    /// The count of SQL columns without any null indicator column
    size_t getColumnCount() const;

    const std::vector<std::string> & getColumnNames() const;

    BitmapTable & getNullIndicatorTable() { return _nullIndicatorTable; }

    BitmapTable & getBranchBitmap() { return _branchBitmap; }

    Database & getDatabase() const;

    const std::vector<Sql::SqlType> & getTupleType() const;

    size_t size() const;

private:
//    std::string _name;
    std::unordered_map<
            std::string,
            std::pair<std::unique_ptr<ColumnInformation>, std::unique_ptr<Vector>>> _columns;
    std::vector<std::string> _columnNames;

    std::vector<std::unique_ptr<Vector>> _columns;

    BitmapTable _nullIndicatorTable;
    BitmapTable _branchBitmap;
public: // TODO
    // points to the first tuple within the version chain of each tuple
    std::vector<void *> mv_begin;
    // next pointer as seen from the master branch (the actual column)
    std::vector<void *> mv_next;

    std::vector<void *> dangling_chains;
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
    std::string name;
    Branch * parent;
};

//-----------------------------------------------------------------------------
// Database

class Database {
public:
    void addTable(std::unique_ptr<Table> table, const std::string & name);

    Table * getTable(const std::string & tableName);

    branch_id_t getLargesBranchId() const;

private:
    std::unordered_map<std::string, std::unique_ptr<Table>> _tables;
    std::unordered_map<std::string, std::unique_ptr<Index>> _indexes;

    std::unordered_map<branch_id_t, std::unique_ptr<Branch>> _branches;
    branch_id_t _next_branch_id;
};
