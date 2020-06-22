//
// Created by josef on 04.01.17.
//

#include <llvm/IR/TypeBuilder.h>

#include "codegen/CodeGen.hpp"
#include "foundations/loader.hpp"
#include "include/tardisdb/semanticAnalyser/SemanticAnalyser.hpp"
#include "algebra/translation.hpp"
#include "queryExecutor/queryExecutor.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

class QueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        db = std::make_unique<Database>();
    }

    void TearDown() override {

    }

    static void insertIntoCallbackHandler(Native::Sql::SqlTuple *tuple) {
        bool isIntegerFirstOrder = tuple->values[0]->type.typeID == Sql::SqlType::TypeID::IntegerID;
        ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 0 : 1]->equals(Native::Sql::Integer(1)));
        ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 1 : 0]->equals(Native::Sql::Numeric(Sql::getNumericTy(6,2,false),400)));
    }

    static void updateCallbackHandler(Native::Sql::SqlTuple *tuple) {
        bool isIntegerFirstOrder = tuple->values[0]->type.typeID == Sql::SqlType::TypeID::IntegerID;
        ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 0 : 1]->equals(Native::Sql::Integer(1)));
        ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 1 : 0]->equals(Native::Sql::Numeric(Sql::getNumericTy(6,2,false),500)));
    }

    static void insertIntoVersionCallbackHandler(Native::Sql::SqlTuple *tuple) {
        bool isIntegerFirstOrder = tuple->values[0]->type.typeID == Sql::SqlType::TypeID::IntegerID;
        ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 0 : 1]->equals(Native::Sql::Integer(1)) || tuple->values[isIntegerFirstOrder ? 0 : 1]->equals(Native::Sql::Integer(2)));

        if (tuple->values[isIntegerFirstOrder ? 0 : 1]->equals(Native::Sql::Integer(1))) {
            ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 1 : 0]->equals(Native::Sql::Numeric(Sql::getNumericTy(6,2,false),400)));
        } else {
            ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 1 : 0]->equals(Native::Sql::Numeric(Sql::getNumericTy(6,2,false),300)));
        }
    }

    std::unique_ptr<Database> db;
};


TEST_F(QueryTest, CreateTable) {
    QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 6 , 2 ) NOT NULL );",*db);

    ASSERT_TRUE(db->hasTable("professoren"));
    Table* professorenTable = db->getTable("professoren");
    ASSERT_NE(professorenTable, nullptr);
    EXPECT_EQ(professorenTable->getColumnCount(),3);

    ci_p_t idCI = professorenTable->getCI("id");
    ASSERT_NE(idCI, nullptr);
    ASSERT_TRUE(Sql::equals(idCI->type,Sql::getIntegerTy(false),Sql::SqlTypeEqualsMode::Full));
    ASSERT_NO_THROW(db->getTable("professoren")->getColumn("id"));

    ci_p_t nameCI = professorenTable->getCI("name");
    ASSERT_NE(nameCI, nullptr);
    ASSERT_TRUE(Sql::equals(nameCI->type,Sql::getVarcharTy(15,false),Sql::SqlTypeEqualsMode::Full));
    ASSERT_NO_THROW(db->getTable("professoren")->getColumn("name"));

    ci_p_t rangCI = professorenTable->getCI("rang");
    ASSERT_NE(rangCI, nullptr);
    ASSERT_TRUE(Sql::equals(rangCI->type,Sql::getNumericTy(6,2,false),Sql::SqlTypeEqualsMode::Full));
    ASSERT_NO_THROW(db->getTable("professoren")->getColumn("rang"));

    //QueryCompiler::compileAndExecute("insert into professoren version hello ( name , rang ) VALUES ( 'kemper' , 4 );",*db);
    //QueryCompiler::compileAndExecute("select rang, name from professoren p;",*db, (void*) &callbackHandler);
    //QueryCompiler::compileAndExecute("select name , rang from professoren version hello p;",*db);
}

TEST_F(QueryTest, CheckoutBranch) {
    QueryCompiler::compileAndExecute("checkout branch hello from master;",*db);

    ASSERT_NO_THROW(db->_branchMapping["hello"]);
    branch_id_t id = db->_branchMapping["hello"];
    Branch* branch = db->_branches[id].get();

    ASSERT_EQ(branch->name,"hello");
    ASSERT_EQ(branch->id,id);
    ASSERT_EQ(branch->parent_id,master_branch_id);
}

TEST_F(QueryTest, InsertInto) {
    QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 6 , 2 ) NOT NULL );",*db);
    QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 1, 'kemper' , 4 );",*db);
    QueryCompiler::compileAndExecute("select id, rang from professoren p;",*db, (void*) &insertIntoCallbackHandler);
}

TEST_F(QueryTest, InsertIntoVersion) {
    QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 6 , 2 ) NOT NULL );",*db);
    QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 1, 'kemper' , 4 );",*db);
    QueryCompiler::compileAndExecute("checkout branch hello from master;",*db);
    QueryCompiler::compileAndExecute("INSERT INTO professoren VERSION hello ( id, name , rang ) VALUES ( 2, 'professor2' , 3 );",*db);
    QueryCompiler::compileAndExecute("select id , rang from professoren version hello p;",*db, (void*) &insertIntoVersionCallbackHandler);
    QueryCompiler::compileAndExecute("select id, rang from professoren p;",*db, (void*) &insertIntoCallbackHandler);
}

TEST_F(QueryTest, Update) {
    QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 6 , 2 ) NOT NULL );",*db);
    QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 1, 'kemper' , 4 );",*db);
    QueryCompiler::compileAndExecute("UPDATE professoren p SET rang = 5 WHERE id = 1 ;",*db);
    QueryCompiler::compileAndExecute("select id, rang from professoren p;",*db, (void*) &updateCallbackHandler);
}

TEST_F(QueryTest, UpdateVersion) {
    QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 6 , 2 ) NOT NULL );",*db);
    QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 1, 'kemper' , 4 );",*db);
    QueryCompiler::compileAndExecute("checkout branch hello from master;",*db);
    QueryCompiler::compileAndExecute("UPDATE professoren VERSION hello p SET rang = 5 WHERE id = 1 ;",*db);
    QueryCompiler::compileAndExecute("select id, rang from professoren p;",*db, (void*) &insertIntoCallbackHandler);
    QueryCompiler::compileAndExecute("select id, rang from professoren VERSION hello p;",*db, (void*) &updateCallbackHandler);
}