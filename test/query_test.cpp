#include <llvm/IR/TypeBuilder.h>

#include "codegen/CodeGen.hpp"
#include "foundations/loader.hpp"
#include "include/tardisdb/semanticAnalyser/SemanticAnalyser.hpp"
#include "algebra/translation.hpp"
#include "queryExecutor/queryExecutor.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {
    class QueryTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
        }

        void TearDown() override {

        }

        static void stateProfessor2CallbackHandler(Native::Sql::SqlTuple *tuple) {
            bool isIntegerFirstOrder = tuple->values[0]->type.typeID == Sql::SqlType::TypeID::IntegerID;
            ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 0 : 1]->equals(Native::Sql::Integer(2)));
            ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 1 : 0]->equals(Native::Sql::Numeric(Sql::getNumericTy(32,8,false),300000000)));
        }


        static void stateKemperCallbackHandler(Native::Sql::SqlTuple *tuple) {
            bool isIntegerFirstOrder = tuple->values[0]->type.typeID == Sql::SqlType::TypeID::IntegerID;
            ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 0 : 1]->equals(Native::Sql::Integer(1)));
            ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 1 : 0]->equals(Native::Sql::Numeric(Sql::getNumericTy(32,8,false),400000000)));
        }

        static void stateKemperUpdatedCallbackHandler(Native::Sql::SqlTuple *tuple) {
            bool isIntegerFirstOrder = tuple->values[0]->type.typeID == Sql::SqlType::TypeID::IntegerID;
            ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 0 : 1]->equals(Native::Sql::Integer(1)));
            ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 1 : 0]->equals(Native::Sql::Numeric(Sql::getNumericTy(32,8,false),500000000)));
        }

        static void stateKemperProfessor2CallbackHandler(Native::Sql::SqlTuple *tuple) {
            bool isIntegerFirstOrder = tuple->values[0]->type.typeID == Sql::SqlType::TypeID::IntegerID;
            ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 0 : 1]->equals(Native::Sql::Integer(1)) || tuple->values[isIntegerFirstOrder ? 0 : 1]->equals(Native::Sql::Integer(2)));

            if (tuple->values[isIntegerFirstOrder ? 0 : 1]->equals(Native::Sql::Integer(1))) {
                ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 1 : 0]->equals(Native::Sql::Numeric(Sql::getNumericTy(32,8,false),400000000)));
            } else {
                ASSERT_TRUE(tuple->values[isIntegerFirstOrder ? 1 : 0]->equals(Native::Sql::Numeric(Sql::getNumericTy(32,8,false),300000000)));
            }
        }

        std::unique_ptr<Database> db;
    };


    TEST_F(QueryTest, CreateTable) {
#if USE_HYRISE
        QueryCompiler::compileAndExecute("create table professoren ( id INT NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang FLOAT NOT NULL );",*db);
#else
        QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 32 , 8 ) NOT NULL );",*db);
#endif
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
        ASSERT_TRUE(Sql::equals(rangCI->type,Sql::getNumericTy(32,8,false),Sql::SqlTypeEqualsMode::Full));
        ASSERT_NO_THROW(db->getTable("professoren")->getColumn("rang"));
    }

    TEST_F(QueryTest, CheckoutBranch) {
        QueryCompiler::compileAndExecute("create branch hello from master;",*db);

        ASSERT_NO_THROW(db->_branchMapping["hello"]);
        branch_id_t id = db->_branchMapping["hello"];
        Branch* branch = db->_branches[id].get();

        ASSERT_EQ(branch->name,"hello");
        ASSERT_EQ(branch->id,id);
        ASSERT_EQ(branch->parent_id,master_branch_id);
    }

    TEST_F(QueryTest, InsertIntoSelect) {
#if USE_HYRISE
        QueryCompiler::compileAndExecute("create table professoren ( id INT NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang FLOAT NOT NULL );",*db);
#else
        QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 32 , 8 ) NOT NULL );",*db);
#endif
        QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 1, 'kemper' , 4 );",*db);
        QueryCompiler::compileAndExecute("select id, rang from professoren p;",*db, (void*) &stateKemperCallbackHandler);
    }

    TEST_F(QueryTest, SelectJoin) {
#if USE_HYRISE
        QueryCompiler::compileAndExecute("create table professoren ( id INT NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang FLOAT NOT NULL );",*db);
#else
        QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 32 , 8 ) NOT NULL );",*db);
#endif
        QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 1, 'kemper' , 4 );",*db);
        QueryCompiler::compileAndExecute("select id, rang from professoren p;",*db, (void*) &stateKemperCallbackHandler);
    }

    TEST_F(QueryTest, InsertIntoVersion) {
#if USE_HYRISE
        QueryCompiler::compileAndExecute("create table professoren ( id INT NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang FLOAT NOT NULL );",*db);
#else
        QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 32 , 8 ) NOT NULL );",*db);
#endif
        QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 1, 'kemper' , 4 );",*db);
        QueryCompiler::compileAndExecute("create branch hello from master;",*db);
        QueryCompiler::compileAndExecute("INSERT INTO professoren VERSION hello ( id, name , rang ) VALUES ( 2, 'professor2' , 3 );",*db);
        QueryCompiler::compileAndExecute("select id , rang from professoren version hello p;",*db, (void*) &stateKemperProfessor2CallbackHandler);
        QueryCompiler::compileAndExecute("select id, rang from professoren p;",*db, (void*) &stateKemperCallbackHandler);
    }

    TEST_F(QueryTest, Update) {
#if USE_HYRISE
        QueryCompiler::compileAndExecute("create table professoren ( id INT NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang FLOAT NOT NULL );",*db);
#else
        QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 32 , 8 ) NOT NULL );",*db);
#endif
        QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 1, 'kemper' , 4 );",*db);
        QueryCompiler::compileAndExecute("UPDATE professoren SET rang = 5 WHERE id = 1 ;",*db);
        QueryCompiler::compileAndExecute("select id, rang from professoren p;",*db, (void*) &stateKemperUpdatedCallbackHandler);
    }

    TEST_F(QueryTest, UpdateBranchVersion) {
#if USE_HYRISE
        QueryCompiler::compileAndExecute("create table professoren ( id INT NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang FLOAT NOT NULL );",*db);
#else
        QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 32 , 8 ) NOT NULL );",*db);
#endif
        QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 1, 'kemper' , 4 );",*db);
        QueryCompiler::compileAndExecute("create branch hello from master;",*db);
        QueryCompiler::compileAndExecute("UPDATE professoren VERSION hello SET rang = 5 WHERE id = 1 ;",*db);
        QueryCompiler::compileAndExecute("select id, rang from professoren p;",*db, (void*) &stateKemperCallbackHandler);
        QueryCompiler::compileAndExecute("select id, rang from professoren VERSION hello p;",*db, (void*) &stateKemperUpdatedCallbackHandler);
    }

    TEST_F(QueryTest, UpdateMasterVersion) {
#if USE_HYRISE
        QueryCompiler::compileAndExecute("create table professoren ( id INT NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang FLOAT NOT NULL );",*db);
#else
        QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 32 , 8 ) NOT NULL );",*db);
#endif
        QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 1, 'kemper' , 4 );",*db);
        QueryCompiler::compileAndExecute("create branch hello from master;",*db);
        QueryCompiler::compileAndExecute("UPDATE professoren SET rang = 5 WHERE id = 1 ;",*db);
        QueryCompiler::compileAndExecute("select id, rang from professoren p;",*db, (void*) &stateKemperUpdatedCallbackHandler);
        QueryCompiler::compileAndExecute("select id, rang from professoren VERSION hello p;",*db, (void*) &stateKemperCallbackHandler);
    }

    TEST_F(QueryTest, Delete) {
#if USE_HYRISE
        QueryCompiler::compileAndExecute("create table professoren ( id INT NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang FLOAT NOT NULL );",*db);
#else
        QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 32 , 8 ) NOT NULL );",*db);
#endif
        QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 1, 'kemper' , 4 );",*db);
        QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 2, 'professor2' , 3 );",*db);
        QueryCompiler::compileAndExecute("DELETE FROM professoren WHERE id = 1 ;",*db);
        QueryCompiler::compileAndExecute("select id, rang from professoren p;",*db, (void*) &stateProfessor2CallbackHandler);
    }

    TEST_F(QueryTest, DeleteBranchVisisbleVersion) {
#if USE_HYRISE
        QueryCompiler::compileAndExecute("create table professoren ( id INT NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang FLOAT NOT NULL );",*db);
#else
        QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 32 , 8 ) NOT NULL );",*db);
#endif
        QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 1, 'kemper' , 4 );",*db);
        QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 2, 'professor2' , 3 );",*db);
        QueryCompiler::compileAndExecute("create branch hello from master;",*db);
        QueryCompiler::compileAndExecute("DELETE FROM professoren WHERE id = 1 ;",*db);
        QueryCompiler::compileAndExecute("select id, rang from professoren version hello p;",*db, (void*) &stateKemperProfessor2CallbackHandler);
        QueryCompiler::compileAndExecute("select id, rang from professoren p;",*db, (void*) &stateProfessor2CallbackHandler);
    }

    TEST_F(QueryTest, DeleteInBranchVersion) {
#if USE_HYRISE
        QueryCompiler::compileAndExecute("create table professoren ( id INT NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang FLOAT NOT NULL );",*db);
#else
        QueryCompiler::compileAndExecute("create table professoren ( id INTEGER NOT NULL, name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 32 , 8 ) NOT NULL );",*db);
#endif
        QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 1, 'kemper' , 4 );",*db);
        QueryCompiler::compileAndExecute("INSERT INTO professoren ( id, name , rang ) VALUES ( 2, 'professor2' , 3 );",*db);
        QueryCompiler::compileAndExecute("create branch hello from master;",*db);
        QueryCompiler::compileAndExecute("DELETE FROM professoren version hello WHERE id = 1 ;",*db);
        QueryCompiler::compileAndExecute("select id, rang from professoren version hello p;",*db, (void*) &stateProfessor2CallbackHandler);
        QueryCompiler::compileAndExecute("select id, rang from professoren p;",*db, (void*) &stateKemperProfessor2CallbackHandler);
    }
}

