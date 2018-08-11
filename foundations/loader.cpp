
#include "loader.hpp"

#include <fstream>
#include <llvm/IR/TypeBuilder.h>

#include "codegen/CodeGen.hpp"
#include "foundations/utils.hpp"
#include "foundations/Database.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"

using namespace Sql;

void genLoadValue(cg_ptr8_t str, cg_size_t length, SqlType type, Vector & column)
{
    auto & codeGen = getThreadLocalCodeGen();

    // this is fine for both types of null indicators
    // - there is no space within the Vector in the case of an external null indicator (see NullIndicatorTable)
    // - it wont matter for internal null indicators
    SqlType notNullableType = toNotNullableTy(type);

    // parse value
    value_op_t value = Value::castString(str, length, notNullableType);
    cg_voidptr_t destPtr = genVectoBackCall(cg_voidptr_t::fromRawPointer(&column));

    // cast destination pointer
    llvm::Type * sqlValuePtrTy = llvm::PointerType::getUnqual(toLLVMTy(notNullableType));
    llvm::Value * sqlValuePtr = codeGen->CreatePointerCast(destPtr.getValue(), sqlValuePtrTy);

    value->store(sqlValuePtr);
}

struct RowItem {
    size_t length;
    const char * str;
};

static llvm::Type * getRowItemTy()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    std::vector<llvm::Type *> members(2);

    // RowItem struct members:
    members[0] = cg_size_t::getType();
    members[1] = cg_ptr8_t::getType();

    llvm::StructType * rowItemTy = llvm::StructType::get(context, false);
    rowItemTy->setBody(members);

    return rowItemTy;
}

static inline llvm::Type * getRowTy(size_t columnCount, llvm::Type * rowItemTy)
{
    llvm::Type * rowTy = llvm::ArrayType::get(rowItemTy, columnCount);
    return rowTy;
}

llvm::Function * genLoadRowFunction(Table & table)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    // prototype: loadRow(RowItem items[])
    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (void *), false>::get(context);
    FunctionGen funcGen(moduleGen, "loadRow", funcTy);

    // get types
    llvm::Type * rowItemTy = getRowItemTy();
    llvm::Type * rowTy = getRowTy(table.getColumnCount(), rowItemTy);

    // cast row pointer
    llvm::Value * rawPtr = funcGen.getArg(0);
    llvm::Value * rowPtr = codeGen->CreateBitCast(rawPtr, llvm::PointerType::getUnqual(rowTy));

    // load each value within the row
    size_t i = 0;
    for (const std::string & column : table.getColumnNames()) {
        ci_p_t ci = table.getCI(column);

        llvm::Value * itemPtr = codeGen->CreateGEP(rowTy, rowPtr, { cg_size_t(0ul), cg_size_t(i) });

        llvm::Value * lengthPtr = codeGen->CreateStructGEP(rowItemTy, itemPtr, 0);
        llvm::Value * strPtr = codeGen->CreateStructGEP(rowItemTy, itemPtr, 1);

        llvm::Value * length = codeGen->CreateLoad(lengthPtr);
        llvm::Value * str = codeGen->CreateLoad(strPtr);

        genLoadValue(cg_ptr8_t(str), cg_size_t(length), ci->type, *ci->column);

        i += 1;
    }

    return funcGen.getFunction();
}

void loadTable(std::istream & stream, Table & table)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::Function * loadFun = genLoadRowFunction(table);

    // compile
    llvm::EngineBuilder eb( moduleGen.finalizeModule() );
    auto ee = std::unique_ptr<llvm::ExecutionEngine>( eb.create() );
    ee->finalizeObject();

    // lookup compiled function
    typedef void (* FunPtr)(void *);
    FunPtr f = reinterpret_cast<FunPtr>(ee->getPointerToFunction(loadFun));

    // load each row
    std::string rowStr;
    std::vector<RowItem> row(table.getColumnCount());
    while (std::getline(stream, rowStr)) {
        table.addRow();

        std::vector<std::string> items = split(rowStr, '|');
        assert(row.size() == items.size());

        size_t i = 0;
        for (const std::string & itemStr : items) {
            RowItem & item = row[i];
            item.length = itemStr.size();
            item.str = itemStr.c_str();
            i += 1;
        }

        // load row
        f(row.data());
    }
}

std::unique_ptr<Database> loadUniDb()
{
    auto uniDb = std::make_unique<Database>();

    // load professoren.tbl
    {
        ModuleGen moduleGen("LoadTableModule");

        auto professoren = std::make_unique<Table>();
        professoren->addColumn("persnr", Sql::getIntegerTy());
        professoren->addColumn("name", Sql::getVarcharTy(20));
        professoren->addColumn("rang", Sql::getVarcharTy(2));
        professoren->addColumn("raum", Sql::getIntegerTy());

        std::ifstream fs("tables/uni/professoren.tbl");
        if (!fs) { throw std::runtime_error("file not found"); }

        loadTable(fs, *professoren);
        uniDb->addTable(std::move(professoren), "professoren");
    }

    // load studenten.tbl
    {
        ModuleGen moduleGen("LoadTableModule");

        auto studenten = std::make_unique<Table>();
        studenten->addColumn("matrnr", Sql::getIntegerTy());
        studenten->addColumn("name", Sql::getVarcharTy(20));
//        studenten->addColumn("semester", Sql::getIntegerTy(true));
        studenten->addColumn("semester", Sql::getIntegerTy());

        std::ifstream fs("tables/uni/studenten.tbl");
        if (!fs) { throw std::runtime_error("file not found"); }

        loadTable(fs, *studenten);
        uniDb->addTable(std::move(studenten), "studenten");
    }

    // load vorlesungen.tbl
    {
        ModuleGen moduleGen("LoadTableModule");

        auto vorlesungen = std::make_unique<Table>();
        vorlesungen->addColumn("vorlnr", Sql::getIntegerTy());
        vorlesungen->addColumn("titel", Sql::getVarcharTy(40));
        vorlesungen->addColumn("sws", Sql::getIntegerTy());
        vorlesungen->addColumn("gelesenvon", Sql::getIntegerTy());

        std::ifstream fs("tables/uni/vorlesungen.tbl");
        if (!fs) { throw std::runtime_error("file not found"); }

        loadTable(fs, *vorlesungen);
        uniDb->addTable(std::move(vorlesungen), "vorlesungen");
    }

    // load voraussetzen.tbl
    {
        ModuleGen moduleGen("LoadTableModule");

        auto voraussetzen = std::make_unique<Table>();
        voraussetzen->addColumn("vorgaenger", Sql::getIntegerTy());
        voraussetzen->addColumn("nachfolger", Sql::getIntegerTy());

        std::ifstream fs("tables/uni/voraussetzen.tbl");
        if (!fs) { throw std::runtime_error("file not found"); }

        loadTable(fs, *voraussetzen);
        uniDb->addTable(std::move(voraussetzen), "voraussetzen");
    }

    // load hoeren.tbl
    {
        ModuleGen moduleGen("LoadTableModule");

        auto hoeren = std::make_unique<Table>();
        hoeren->addColumn("matrnr", Sql::getIntegerTy());
        hoeren->addColumn("vorlnr", Sql::getIntegerTy());

        std::ifstream fs("tables/uni/hoeren.tbl");
        if (!fs) { throw std::runtime_error("file not found"); }

        loadTable(fs, *hoeren);
        uniDb->addTable(std::move(hoeren), "hoeren");
    }

    // load assistenten.tbl
    {
        ModuleGen moduleGen("LoadTableModule");

        auto assistenten = std::make_unique<Table>();
        assistenten->addColumn("persnr", Sql::getIntegerTy());
        assistenten->addColumn("name", Sql::getVarcharTy(20));
        assistenten->addColumn("fachgebiet", Sql::getVarcharTy(40));
        assistenten->addColumn("boss", Sql::getIntegerTy());

        std::ifstream fs("tables/uni/assistenten.tbl");
        if (!fs) { throw std::runtime_error("file not found"); }

        loadTable(fs, *assistenten);
        uniDb->addTable(std::move(assistenten), "assistenten");
    }

    // load pruefen.tbl
    {
        ModuleGen moduleGen("LoadTableModule");

        auto pruefen = std::make_unique<Table>();
        pruefen->addColumn("matrnr", Sql::getIntegerTy());
        pruefen->addColumn("vorlnr", Sql::getIntegerTy());
        pruefen->addColumn("persnr", Sql::getIntegerTy());
        pruefen->addColumn("note", Sql::getNumericTy(2, 1));

        std::ifstream fs("tables/uni/pruefen.tbl");
        if (!fs) { throw std::runtime_error("file not found"); }

        loadTable(fs, *pruefen);
        uniDb->addTable(std::move(pruefen), "pruefen");
    }

    return uniDb;
}
