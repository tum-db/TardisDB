#include "semanticAnalyser/SemanticAnalyser.hpp"

#include <map>

namespace semanticalAnalysis {

using sqlExpressions::Node;
using Kind = Node::Kind;
using Resolver = std::function<iu_p_t(const std::string &, const std::string &)>;
using LogicalWindow = Algebra::Logical::Window;
using ID = Sql::SqlType::TypeID;

bool containsWindow(const Node & n)
{
    if (n.kind == Kind::Window) return true;
    for (auto & child : n.children) if (containsWindow(child)) return true;
    return false;
}

namespace {

/// A text that equal expressions share
std::string key(const Node & n)
{
    std::string out = std::to_string(static_cast<int>(n.kind)) + '\x02' + n.name + '\x02' + n.table + '\x02' + n.value + (n.quoted ? "'" : "") + "(";
    for (auto & child : n.children) out += key(child) + ",";
    return out + ")";
}

bool isInteger(Sql::SqlType t) { return t.typeID == ID::IntegerID || t.typeID == ID::LongIntegerID; }

bool isNumber(Sql::SqlType t) { return isInteger(t) || t.typeID == ID::NumericID || t.typeID == ID::DoubleID; }

LogicalWindow::BoundKind boundKind(const std::string & text)
{
    using B = LogicalWindow::BoundKind;
    if (text == "unbounded preceding") return B::UnboundedPreceding;
    if (text == "preceding") return B::Preceding;
    if (text == "following") return B::Following;
    if (text == "unbounded following") return B::UnboundedFollowing;
    return B::CurrentRow;
}

} // namespace

Resolver SelectAnalyser::constructWindows(std::vector<Node> & expressions, Resolver resolve)
{
    using F = LogicalWindow::Function;
    using B = LogicalWindow::BoundKind;
    ExpressionAnalyser inputs(resolve);
    std::vector<Mapping> mappings;
    // an input of a function: an attribute, computed below the windows unless the expression is one
    auto attribute = [&](Expressions::exp_op_t e) -> iu_p_t {
        if (auto * identifier = dynamic_cast<Expressions::Identifier *>(e.get())) return identifier->_iu;
        iu_p_t iu = _context.iuFactory.createIU(e->getType());
        mappings.push_back({iu, std::move(e)});
        return iu;
    };
    auto input = [&](const Node & n) {
        if (containsWindow(n)) throw semantic_sql_error("window function calls cannot be nested");
        Expressions::exp_op_t e = inputs.analyse(n);
        if (e->getType().typeID == ID::UnknownID) {
            const Sql::SqlType text = Sql::getTextTy(true);
            e = inputs.analyse(n, &text);
        }
        return attribute(std::move(e));
    };
    auto integer = [&](const Node & n, const std::string & what) {
        if (containsWindow(n)) throw semantic_sql_error("window function calls cannot be nested");
        const Sql::SqlType bigint = Sql::getLongIntegerTy(true);
        Expressions::exp_op_t e = inputs.analyse(n, &bigint);
        if (e->getType().typeID == ID::UnknownID) e = inputs.analyse(n, &bigint);
        if (!isInteger(e->getType())) throw semantic_sql_error("argument of " + what + " must be an integer, not " + Sql::getName(Sql::toNotNullableTy(e->getType())));
        return attribute(std::move(e));
    };
    auto orderable = [](iu_p_t iu, const std::string & what) {
        if (iu->sqlType.typeID == ID::ArrayID) throw semantic_sql_error(what + " of a window takes no arrays");
        return iu;
    };

    struct Group {
        std::vector<iu_p_t> partition;
        std::vector<LogicalWindow::OrderKey> order;
        std::vector<LogicalWindow::Call> calls;
    };
    std::vector<Group> groups;
    std::map<std::string, size_t> groupOf;
    std::vector<iu_p_t> results;

    auto call = [&](const Node & n, const Group & group) {
        const std::string & f = n.name;
        const auto & args = n.children;
        auto arity = [&](size_t low, size_t high) {
            if (args.size() < low || args.size() > high) {
                throw semantic_sql_error("window function " + f + " takes " + std::to_string(low) + (high > low ? " to " + std::to_string(high) : "") +
                                         " argument" + (high == 1 ? "" : "s"));
            }
        };
        LogicalWindow::Call c;
        Sql::SqlType result;
        static const std::map<std::string, F> ranking = {{"row_number", F::RowNumber}, {"rank", F::Rank}, {"dense_rank", F::DenseRank},
                                                         {"percent_rank", F::PercentRank}, {"cume_dist", F::CumeDist}};
        static const std::map<std::string, F> values = {{"lag", F::Lag}, {"lead", F::Lead}, {"first_value", F::FirstValue},
                                                        {"last_value", F::LastValue}, {"nth_value", F::NthValue}};
        static const std::map<std::string, F> aggregates = {{"count", F::Count}, {"sum", F::Sum}, {"avg", F::Avg}, {"min", F::Min}, {"max", F::Max}};
        if (n.value == "*" && f != "count") throw semantic_sql_error("window function " + f + " takes no *");
        if (ranking.count(f)) {
            arity(0, 0);
            c.function = ranking.at(f);
            result = c.function == F::PercentRank || c.function == F::CumeDist ? Sql::getDoubleTy() : Sql::getLongIntegerTy();
        } else if (f == "ntile") {
            arity(1, 1);
            c.function = F::Ntile;
            c.arguments.push_back(integer(args[0], "ntile"));
            result = Sql::getIntegerTy(true);
        } else if (values.count(f)) {
            c.function = values.at(f);
            if (c.function == F::Lag || c.function == F::Lead) arity(1, 3);
            else arity(c.function == F::NthValue ? 2 : 1, c.function == F::NthValue ? 2 : 1);
            iu_p_t value = input(args[0]);
            c.arguments.push_back(value);
            result = Sql::toNullableTy(value->sqlType);
            if (args.size() > 1) c.arguments.push_back(integer(args[1], f));
            if (args.size() > 2) {
                // the default in the type of the value
                if (containsWindow(args[2])) throw semantic_sql_error("window function calls cannot be nested");
                c.arguments.push_back(attribute(inputs.analyseValue(args[2], result, f)));
            }
        } else if (aggregates.count(f)) {
            const bool star = f == "count" && n.value == "*";
            arity(star ? 0 : 1, star ? 0 : 1);
            c.function = star ? F::CountAll : aggregates.at(f);
            iu_p_t value = nullptr;
            if (!star) {
                Node argument = args[0];
                if (f == "min" || f == "max") {
                    // the least or greatest char or varchar as text, as the aggregation computes it
                    if (containsWindow(argument)) throw semantic_sql_error("window function calls cannot be nested");
                    const ID id = inputs.analyse(argument)->getType().typeID;
                    if (id == ID::CharID || id == ID::VarcharID) {
                        Node text;
                        text.kind = Kind::Cast;
                        text.name = "text";
                        text.children = {argument};
                        argument = text;
                    }
                }
                value = input(argument);
                c.arguments.push_back(value);
            }
            const Sql::SqlType type = star ? Sql::getIntegerTy() : Sql::toNotNullableTy(value->sqlType);
            using AF = Algebra::Logical::Aggregation::Function;
            static const std::map<F, AF> same = {{F::CountAll, AF::CountAll}, {F::Count, AF::Count}, {F::Sum, AF::Sum}, {F::Avg, AF::Avg},
                                                 {F::Min, AF::Min}, {F::Max, AF::Max}};
            if (type.typeID == ID::ArrayID) throw semantic_sql_error("window function " + f + " takes no arrays");
            if (!Algebra::Logical::Aggregation::aggregateType(same.at(c.function), type, result)) {
                if (c.function == F::Sum || c.function == F::Avg) throw semantic_sql_error(f + " takes numbers, not " + Sql::getName(type));
                throw semantic_sql_error("function " + f + "(" + Sql::getName(type) + ") does not exist");
            }
        } else {
            throw semantic_sql_error("window function " + f + " does not exist");
        }

        // the frame: by default from the start of the partition to the last peer of the row
        const sqlExpressions::Window & w = *n.window;
        if (!w.mode.empty()) {
            c.mode = w.mode == "rows" ? LogicalWindow::Mode::Rows : w.mode == "groups" ? LogicalWindow::Mode::Groups : LogicalWindow::Mode::Range;
            c.start.kind = boundKind(w.start);
            c.end.kind = boundKind(w.end);
            if (c.start.kind == B::UnboundedFollowing) throw semantic_sql_error("frame start cannot be UNBOUNDED FOLLOWING");
            if (c.end.kind == B::UnboundedPreceding) throw semantic_sql_error("frame end cannot be UNBOUNDED PRECEDING");
            if (c.start.kind == B::CurrentRow && c.end.kind == B::Preceding) throw semantic_sql_error("frame starting from current row cannot have preceding rows");
            if (c.start.kind == B::Following && (c.end.kind == B::Preceding || c.end.kind == B::CurrentRow)) {
                throw semantic_sql_error("frame starting from following row cannot have preceding rows");
            }
            if (c.mode == LogicalWindow::Mode::Groups && group.order.empty()) throw semantic_sql_error("GROUPS mode requires an ORDER BY clause");
            auto offset = [&](const std::vector<Node> & nodes) -> iu_p_t {
                if (nodes.empty()) return nullptr;
                if (c.mode != LogicalWindow::Mode::Range) return integer(nodes[0], w.mode == "rows" ? "ROWS" : "GROUPS");
                if (group.order.size() != 1) throw semantic_sql_error("RANGE with offset PRECEDING/FOLLOWING requires exactly one ORDER BY column");
                const Sql::SqlType keyType = group.order[0].input->sqlType;
                if (!isNumber(keyType)) {
                    throw semantic_sql_error("RANGE with offset PRECEDING/FOLLOWING is not supported for column type " + Sql::getName(Sql::toNotNullableTy(keyType)));
                }
                iu_p_t iu = input(nodes[0]);
                if (!isNumber(iu->sqlType)) throw semantic_sql_error("the offset of RANGE must be a number, not " + Sql::getName(Sql::toNotNullableTy(iu->sqlType)));
                return iu;
            };
            c.start.offset = offset(w.startOffset);
            c.end.offset = offset(w.endOffset);
        }
        c.output = _context.iuFactory.createIU(result);
        return c;
    };

    std::function<Node(const Node &)> rewrite = [&](const Node & n) -> Node {
        if (n.kind != Kind::Window) {
            Node copy = n;
            for (auto & child : copy.children) child = rewrite(child);
            return copy;
        }
        const sqlExpressions::Window & w = *n.window;
        if (!w.name.empty()) throw semantic_sql_error("window \"" + w.name + "\" does not exist");
        // the operator of the partitioning and ordering
        std::string spec;
        for (auto & p : w.partition) spec += key(p) + ";";
        spec += "|";
        for (size_t k = 0; k < w.order.size(); ++k) spec += key(w.order[k]) + (w.descending[k] ? "d" : "a") + (w.nullsFirst[k] ? "f" : "l") + ";";
        auto it = groupOf.find(spec);
        if (it == groupOf.end()) {
            Group g;
            for (auto & p : w.partition) g.partition.push_back(orderable(input(p), "PARTITION BY"));
            for (size_t k = 0; k < w.order.size(); ++k) g.order.push_back({orderable(input(w.order[k]), "ORDER BY"), w.descending[k], w.nullsFirst[k]});
            it = groupOf.emplace(spec, groups.size()).first;
            groups.push_back(std::move(g));
        }
        Group & group = groups[it->second];
        group.calls.push_back(call(n, group));
        results.push_back(group.calls.back().output);
        Node column;
        column.kind = Kind::Column;
        column.name = "\x01" "w" + std::to_string(results.size() - 1);
        return column;
    };
    for (auto & e : expressions) e = rewrite(e);
    if (results.empty()) return resolve;

    if (!mappings.empty()) _context.joinedTree = std::make_unique<Map>(std::move(_context.joinedTree), std::move(mappings));
    for (auto & g : groups) _context.joinedTree = std::make_unique<LogicalWindow>(std::move(_context.joinedTree), g.partition, g.order, g.calls);
    return [resolve, results](const std::string & table, const std::string & name) -> iu_p_t {
        if (name.size() > 2 && name[0] == '\x01' && name[1] == 'w') return results.at(std::stoul(name.substr(2)));
        return resolve(table, name);
    };
}

} // namespace semanticalAnalysis
