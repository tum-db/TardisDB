#include <llvm/ADT/STLExtras.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>

#include "foundations/Database.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include "utils/general.hpp"


#include "native/sql/SqlTuple.hpp"
#include "foundations/InformationUnit.hpp"
#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "queryCompiler/QueryContext.hpp"

#include <boost/algorithm/string.hpp>

#include <unistd.h>
#include <iostream>
#include <string>
#include <cstdlib>
#include <atomic>
#include <unordered_map>
// pistache
#include "pistache/endpoint.h"
#include "pistache/http_header.h"
#include "pistache/http_headers.h"
using namespace Pistache;

static std::string escapeEscapes(const std::string& in) {
    std::stringstream ss;
    size_t idx = 0;
    while (idx < in.size()) {
        if (in[idx] == '\n') {
            ss << '\\' << 'r' << '\\' << 'n';
        } else {
            ss << in[idx];
        }
        ++idx;
    }
    return ss.str();
}
static std::string escapeQuote(const std::string& in) {
    std::stringstream ss;
    size_t idx = 0;
    while (idx < in.size()) {
        if (in[idx] == '"') {
            ss << '\\' << '"';
        } else {
            ss << in[idx];
        }
        ++idx;
    }
    return ss.str();
}

std::unordered_map<uint64_t, std::unique_ptr<Database>> dbs;
std::atomic<uint64_t> dbcounter = 0;
char lectures[] = "{\"content\": [{\"No\": 4052, \"title\": \"logic\", \"ECTS\": 4}]}";
char branches[] = "{\"nodes\": [{\"name\": \"master\", \"id\": 0, \"tuples\": 4}, {\"name\": \"mybranch1\",\"id\": 1,\"tuples\": 15}, {\"name\": \"mybranch2\",\"id\": 2, \"tuples\": 15}, {\"name\": \"mybranch3\",\"id\": 3,\"tuples\": 5}], \"links\": [{\"source\": 0,\"target\": 2,\"weight\": 1},{\"source\": 0,\"target\": 1,\"weight\": 1},{\"source\": 1,\"target\": 3,\"weight\": 1} ]}";

std::stringstream ss;
bool first;
            
void jsonStreamingCallback(Native::Sql::SqlTuple *tuple) {
  if(!first)
     ss << ",";
  ss << "{";
  first = false;
  int column=0; // TODO: replace by real column name
  for (const auto & value : tuple->values) {
      if (column)
         ss << ", ";
      ss << "\""<< column++ << "\":"  "\"" << escapeQuote(toString(*value)) << "\"" ;
  }
  ss << "}";
}

std::string printLineage(std::unique_ptr<Database>& db){
  QueryContext queryContext(*db);
  return semanticalAnalysis::BranchAnalyser(queryContext.analyzingContext).returnJSON();
}

class RestHandler : public Http::Handler {
public:
  HTTP_PROTOTYPE(RestHandler)
  void onRequest(const Http::Request &request, Http::ResponseWriter response) {
    response.headers()
        .add<Http::Header::AccessControlAllowOrigin>("*")
        .add<Http::Header::AccessControlAllowMethods>("GET, POST, PUT, DELETE")
        .add<Http::Header::AccessControlAllowHeaders>("content-type")
        .add<Http::Header::ContentType>(MIME(Application, Json));

    // parse resources for correct db
    std::vector<std::string> resources;
    uint64_t dbindex=0;
    boost::split(resources, request.resource(), boost::is_any_of("/"));
    if (resources.size()>=2)
      dbindex=std::atoi(resources[1].c_str());
    if(dbs.find(dbindex) == dbs.end())
      dbindex=0; //fallback to standard

    // switch over request method
    switch(request.method()){
      case Http::Method::Options:
        // it's a preflight request
        response.send(Http::Code::Ok);
        return;
      case Http::Method::Get:
        response.send(Http::Code::Ok, printLineage(dbs[dbindex]).c_str());
        return;
      case Http::Method::Put:
        dbindex = ++dbcounter;
        dbs.emplace(dbindex,std::make_unique<Database>());
        response.send(Http::Code::Ok,std::to_string(dbindex).c_str());
        return;
      case Http::Method::Post:
        break;
      case Http::Method::Delete:
        fprintf(stderr, "delete: %ld\n", dbindex);
        if(dbindex > 0) dbs.erase(dbindex);
        response.send(Http::Code::Ok);
        return;
      default:
        response.send(Http::Code::Bad_Request, "not supported method");
        return;
    }

    std::string input = request.body(), error="", columnsstring;
    std::vector<std::string> inputs;
    boost::split(inputs, input, boost::is_any_of(";"));
    struct QueryCompiler::BenchmarkResult result;
    for(auto& s: inputs){
      try {
        if(s.size()==0) continue; // no input
        s+=";"; // add terminate symbol
        // reset json printers
        ss.str("");
        first=true;
        result += QueryCompiler::compileAndBenchmark(s,*dbs[dbindex], (void*) jsonStreamingCallback);
        // print result
        columnsstring="";
        int column=0;
        for (auto& i: result.columns){
           if(column>0) columnsstring += ", ";
           columnsstring+= "{\"" + std::to_string(column++) + "\": \"" + i->columnInformation->columnName + "\"}";
        }
      } catch (const std::exception & e) {
        fprintf(stderr, "Exception: %s\n", e.what());
        error += e.what();
        error += "; ";
      } catch (...) {
      }
    }
    std::string output = "{\"columns\": [" + columnsstring + "], \"content\": [" + ss.str() + "]";
    if(error.size()>0)
       output +=", \"errormsg\": \"" + error + "\"";
    output+= ", \"compilationtime\": " + std::to_string(result.llvmCompilationTime/1000) + ", \"executiontime\": " + std::to_string(result.executionTime/1000) +
        ", \"parsingtime\": " + std::to_string(result.parsingTime/1000) + ", \"analysingtime\": " + std::to_string(result.analysingTime/1000) + ", \"translationtime\": "+ std::to_string(result.translationTime/1000) + "}";
    fprintf(stderr, "response: %s\n", output.c_str());
    response.send(Http::Code::Ok, output.c_str());
  }
};

int main(int argc, char **argv) {
  unsigned port = 5000;
  int opt;
  while ((opt = getopt(argc, argv, "p:")) != -1) {
    switch (opt) {
    case 'p':
      port = atoi(optarg);
      break;
    default:
      break;
    }
  }
  // necessary for the LLVM JIT compiler
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();
  dbs.emplace(0,std::make_unique<Database>()); // add a default db

  Pistache::Address addr(Pistache::Ipv4::any(), Pistache::Port(port));
  auto opts = Pistache::Http::Endpoint::options().threads(1).maxRequestSize(1024 * 1024);
  Http::Endpoint server(addr);
  server.init(opts);
  server.setHandler(Http::make_handler<RestHandler>());
  server.serve();
  server.shutdown();

  llvm::llvm_shutdown();

  return 0;
}
