// The registry of the table functions

#include "queryCompiler/TableFunctions.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <shared_mutex>

namespace TableFunctions {

    bool Argument::unknown() const
    {
        return !null && type.typeID == Sql::SqlType::TypeID::TextID && text.size() >= 2 && text.front() == '\'' && text.back() == '\'';
    }

    namespace {
        struct Registry {
            std::shared_mutex latch;
            std::map<std::string, std::unique_ptr<TableFunction>> functions;

            Registry() {
                // the functions of the system
                for (auto * make : {&generateSeries}) {
                    auto function = make();
                    functions[function->name()] = std::move(function);
                }
            }
        };

        Registry & registry()
        {
            static Registry instance;
            return instance;
        }

        std::string lower(const std::string & text)
        {
            std::string result;
            std::transform(text.begin(), text.end(), std::back_inserter(result), ::tolower);
            return result;
        }
    }

    const TableFunction * find(const std::string & name)
    {
        Registry & r = registry();
        std::shared_lock<std::shared_mutex> lock(r.latch);
        auto it = r.functions.find(lower(name));
        return it == r.functions.end() ? nullptr : it->second.get();
    }

    std::vector<std::string> names()
    {
        Registry & r = registry();
        std::shared_lock<std::shared_mutex> lock(r.latch);
        std::vector<std::string> result;
        for (auto & entry : r.functions) result.push_back(entry.first);
        return result;
    }

    bool add(std::unique_ptr<TableFunction> function)
    {
        Registry & r = registry();
        std::unique_lock<std::shared_mutex> lock(r.latch);
        return r.functions.emplace(lower(function->name()), std::move(function)).second;
    }

} // end namespace TableFunctions
