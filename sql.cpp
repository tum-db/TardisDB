#include <chrono>
#include <cstdio>
#include <optional>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <llvm/ADT/STLExtras.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>

#include "foundations/Database.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <readline/readline.h>
#include <readline/history.h>

/// Ctrl-C discards the line being edited instead of quitting the shell
static void sigintHandler(int)
{
    std::cout << std::endl;
    rl_replace_line("", 0);
    rl_on_new_line();
    rl_redisplay();
}

static std::string trim(const std::string & s)
{
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

/// Appends a line to the statement text; \returns Whether the text ends a statement, i.e., contains
/// a semicolon outside string literals and -- comments
static bool appendLine(std::string & text, bool & inString, const std::string & line)
{
    bool complete = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (!inString && c == '-' && i + 1 < line.size() && line[i + 1] == '-') {
            break;
        }
        text += c;
        if (c == '\'') {
            inString = !inString;
        } else if (c == ';' && !inString) {
            complete = true;
        }
    }
    text += '\n';
    return complete;
}

/// Splits the text at semicolons outside string literals; each statement keeps its semicolon
static std::vector<std::string> splitStatements(const std::string & text)
{
    std::vector<std::string> statements;
    std::string current;
    bool inString = false;
    for (char c : text) {
        current += c;
        if (c == '\'') {
            inString = !inString;
        } else if (c == ';' && !inString) {
            if (!trim(current).empty() && trim(current) != ";") {
                statements.push_back(trim(current));
            }
            current.clear();
        }
    }
    if (!trim(current).empty()) {
        statements.push_back(trim(current) + ";");
    }
    return statements;
}

static void handleQuery(Database & db, const std::string & text, bool showTiming)
{
    for (auto & statement : splitStatements(text)) {
        auto start = std::chrono::steady_clock::now();
        std::optional<QueryCompiler::Timing> timing;
        try {
            timing = QueryCompiler::compileAndExecute(statement, db);
        } catch (const std::exception & e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
        if (showTiming) {
            auto end = std::chrono::steady_clock::now();
            const double total = std::chrono::duration<double, std::milli>(end - start).count();
            char line[256];
            if (timing) {
                // compilation: parsing, analysis, translation into LLVM IR and LLVM; execution: the rest
                std::snprintf(line, sizeof(line), "Time: %.3f ms (compilation %.3f ms: parsing %.3f, analysis %.3f, translation %.3f, "
                              "LLVM %.3f; execution %.3f ms)", total, timing->compilation(), timing->parsing, timing->analysis,
                              timing->translation, timing->llvm, timing->execution);
            } else {
                std::snprintf(line, sizeof(line), "Time: %.3f ms", total);
            }
            std::cout << line << std::endl;
        }
    }
}

static bool handleBackslash(Database & db, const std::string & command, bool & showTiming);

/// \returns Whether the file asked to quit
static bool executeFile(Database & db, const std::string & filename, bool & showTiming)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Could not open file: " << filename << std::endl;
        return false;
    }
    std::string text, line;
    bool inString = false;
    while (std::getline(file, line)) {
        std::string trimmed = trim(line);
        if (!inString && trim(text).empty() && (trimmed == "quit" || trimmed == "exit")) {
            return true;
        }
        if (!inString && trimmed.compare(0, 2, "\\i") == 0) {
            // run what precedes the inclusion, then the included file relative to this one
            handleQuery(db, text, showTiming);
            text.clear();
            std::filesystem::path included(trim(trimmed.substr(2)));
            if (included.is_relative()) {
                included = std::filesystem::path(filename).parent_path() / included;
            }
            if (executeFile(db, included.string(), showTiming)) {
                return true;
            }
            continue;
        }
        if (!inString && trim(text).empty() && !trimmed.empty() && trimmed[0] == '\\') {
            if (handleBackslash(db, trimmed, showTiming)) {
                return true;
            }
            continue;
        }
        if (appendLine(text, inString, line)) {
            handleQuery(db, text, showTiming);
            text.clear();
        }
    }
    if (!trim(text).empty()) {
        handleQuery(db, text, showTiming);
    }
    return false;
}

static void printHelp()
{
    std::cout << "Statements end with ';' and may span several lines.\n"
              << "Backslash commands:\n"
              << "  \\q        Quit (also: quit, Ctrl-D)\n"
              << "  \\?        Show this help\n"
              << "  \\i FILE   Execute SQL from file\n"
              << "  \\d        List tables\n"
              << "  \\d TABLE  Describe table\n"
              << "  \\b        List branches\n"
              << "  \\timing   Toggle timing display (compilation and execution)\n";
}

static void listTables(Database & db)
{
    std::vector<std::string> names;
    for (auto & table : db.getTables()) {
        names.push_back(table.first);
    }
    if (names.empty()) {
        std::cout << "No tables." << std::endl;
        return;
    }
    std::sort(names.begin(), names.end());
    std::cout << "Tables:" << std::endl;
    for (auto & name : names) {
        size_t size = db.getTable(name)->size();
        std::cout << "  " << name << " (" << size << (size == 1 ? " tuple)" : " tuples)") << std::endl;
    }
}

static void describeTable(Database & db, const std::string & name)
{
    Table * table = db.getTable(name);
    if (table == nullptr) {
        std::cerr << "Table not found: " << name << std::endl;
        return;
    }
    std::cout << "Table: " << name << std::endl;
    for (auto & column : table->getColumnNames()) {
        auto type = table->getCI(column)->type;
        std::cout << "  " << column << " : " << Sql::getName(type) << (type.nullable ? " (nullable)" : "") << std::endl;
    }
}

static void listBranches(Database & db)
{
    std::vector<const Branch *> branches;
    for (auto & branch : db._branches) {
        branches.push_back(branch.second.get());
    }
    std::sort(branches.begin(), branches.end(), [](auto * a, auto * b) { return a->id < b->id; });
    std::cout << "Branches:" << std::endl;
    for (auto * branch : branches) {
        std::cout << "  " << branch->name;
        if (branch->id == master_branch_id) {
            std::cout << " (root, also: main)";
        } else if (db._branches.count(branch->parent_id) > 0) {
            std::cout << " (from " << db._branches.at(branch->parent_id)->name << ")";
        }
        std::cout << std::endl;
    }
}

/// \returns Whether the shell should quit
static bool handleBackslash(Database & db, const std::string & command, bool & showTiming)
{
    if (command == "\\q") {
        return true;
    } else if (command == "\\?") {
        printHelp();
    } else if (command.compare(0, 2, "\\i") == 0) {
        std::string filename = trim(command.substr(2));
        if (filename.empty()) {
            std::cerr << "Usage: \\i FILE" << std::endl;
        } else {
            return executeFile(db, filename, showTiming);
        }
    } else if (command == "\\d") {
        listTables(db);
    } else if (command.compare(0, 3, "\\d ") == 0) {
        describeTable(db, trim(command.substr(2)));
    } else if (command == "\\b") {
        listBranches(db);
    } else if (command == "\\timing") {
        showTiming = !showTiming;
        std::cout << "Timing is " << (showTiming ? "on" : "off") << "." << std::endl;
    } else {
        std::cerr << "Unknown command: " << command << std::endl;
        std::cerr << "Type \\? for help." << std::endl;
    }
    return false;
}

static void addHistory(const std::string & entry)
{
    HIST_ENTRY * last = history_get(history_base + history_length - 1);
    if (last != nullptr && entry == last->line) {
        return;
    }
    add_history(entry.c_str());
    add_history_time(("#" + std::to_string(std::time(nullptr))).c_str());
}

static void prompt(Database & db)
{
    bool showTiming = false;

    struct sigaction action = {};
    action.sa_handler = sigintHandler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);

    using_history();
    // timestamps separate the entries in the history file, so statements over several lines stay one entry
    history_write_timestamps = 1;
    history_multiline_entries = 1;
    std::string historyFile;
    if (const char * home = std::getenv("HOME")) {
        historyFile = std::string(home) + "/.tardisdb_history";
        read_history(historyFile.c_str());
    }

    while (true) {
        char * line = readline("sql> ");
        if (line == nullptr) { // Ctrl-D
            std::cout << std::endl;
            break;
        }
        std::string input = trim(line);
        free(line);
        if (input.empty()) {
            continue;
        }
        if (input == "quit" || input == "exit") {
            break;
        }
        if (input[0] == '\\') {
            addHistory(input);
            if (handleBackslash(db, input, showTiming)) {
                break;
            }
            continue;
        }

        // read until a line ends the statement; the history keeps the statement as one entry
        std::string text, entry = input;
        bool inString = false;
        bool complete = appendLine(text, inString, input);
        while (!complete) {
            char * continuation = readline("  -> ");
            if (continuation == nullptr) {
                break;
            }
            std::string next(continuation);
            free(continuation);
            entry += "\n" + next;
            complete = appendLine(text, inString, next);
        }
        addHistory(entry);
        handleQuery(db, text, showTiming);
    }

    if (!historyFile.empty()) {
        write_history(historyFile.c_str());
        history_truncate_file(historyFile.c_str(), 1000);
    }
}

int main(int argc, char * argv[])
{
    // necessary for the LLVM JIT compiler
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    {
        auto db = std::make_unique<Database>();
        if (argc > 1) {
            // sql FILE...: execute the files and quit
            bool showTiming = false;
            for (int i = 1; i < argc && !executeFile(*db, argv[i], showTiming); ++i) {
            }
        } else {
            prompt(*db);
        }
    }

    llvm::llvm_shutdown();

    return 0;
}
