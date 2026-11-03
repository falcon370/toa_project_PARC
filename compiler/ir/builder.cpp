#include "ir/builder.hpp"

#include <algorithm>
#include <unordered_map>

#include "common/util.hpp"

namespace parc::ir {
namespace {

using ast::Expr;
using ast::ExprKind;
using ast::Stmt;
using ast::StmtKind;

ast::Type to_ast_type(sem::Type t) {
    ast::Type out;
    out.is_array = t.is_array;
    switch (t.kind) {
        case sem::TypeKind::Float: out.base = ast::BaseType::Float; break;
        case sem::TypeKind::Bool: out.base = ast::BaseType::Bool; break;
        case sem::TypeKind::Void: out.base = ast::BaseType::Void; break;
        default: out.base = ast::BaseType::Int; break;
    }
    return out;
}

VT value_type(sem::Type t) {
    if (t.is_array || t == sem::kNull) return VT::Ref;
    if (t == sem::kFloat) return VT::Float;
    if (t == sem::kBool) return VT::Bool;
    return VT::Int;
}

Operand default_value(ast::Type type) {
    if (type.is_array) return Operand::null();
    switch (type.base) {
        case ast::BaseType::Float: return Operand::of_float(0.0);
        case ast::BaseType::Bool: return Operand::of_bool(false);
        default: return Operand::of_int(0);
    }
}

Op binary_op(Kind kind) {
    switch (kind) {
        case Kind::Plus: return Op::Add;
        case Kind::Minus: return Op::Sub;
        case Kind::Star: return Op::Mul;
        case Kind::Slash: return Op::Div;
        case Kind::Percent: return Op::Mod;
        case Kind::Lt: return Op::Lt;
        case Kind::Le: return Op::Le;
        case Kind::Gt: return Op::Gt;
        case Kind::Ge: return Op::Ge;
        case Kind::Eq: return Op::Eq;
        default: return Op::Ne;
    }
}

class Builder {
public:
    Builder(const ast::Program& program, const sem::Result& types, const std::unordered_set<const ast::For*>& parallel)
        : source_(program), types_(types), parallel_(parallel) {}

    Program run() {
        for (const auto& fn : source_.functions) signatures_.emplace(fn.name, &fn);  // the first definition wins
        for (const auto& fn : source_.functions) build_function(fn);
        return std::move(out_);
    }

private:
    struct LoopTargets {
        int continue_to;
        int break_to;
    };

    const ast::Program& source_;
    const sem::Result& types_;
    const std::unordered_set<const ast::For*>& parallel_;
    std::unordered_map<std::string, const ast::Function*> signatures_;
    Program out_;

    // State for the function being built. Blocks are created with an id, and
    // get their place in the function when code starts going into them.
    Function fn_;
    std::vector<Block> pool_;
    std::vector<int> order_;
    int cur_ = -1;
    std::vector<std::unordered_map<std::string, int>> scopes_;
    std::vector<LoopTargets> loops_;
    std::unordered_map<std::string, int> name_uses_;
    int temps_ = 0;
    int labels_ = 0;
    Pos stmt_;
    int nesting_ = 0;

    // ------------------------------------------------------------ blocks and instructions

    int create(const std::string& base) {
        Block block;
        block.label = pool_.empty() ? base : base + std::to_string(++labels_);
        pool_.push_back(std::move(block));
        return isize(pool_) - 1;
    }

    bool open() const {
        const auto& instrs = pool_[static_cast<size_t>(cur_)].instrs;
        return instrs.empty() || !is_terminator(instrs.back().op);
    }

    // Code now goes into this block. If the previous block just runs off its
    // end, it falls through with an explicit jump.
    void start(int block) {
        if (cur_ >= 0 && open()) jump(block);
        order_.push_back(block);
        cur_ = block;
    }

    Instr& emit(Instr instr) {
        // Code after a return, break or continue goes into a block nothing jumps to.
        if (!open()) start(create("dead"));
        instr.stmt = stmt_;
        auto& instrs = pool_[static_cast<size_t>(cur_)].instrs;
        instrs.push_back(std::move(instr));
        return instrs.back();
    }

    void jump(int target) {
        Instr i;
        i.op = Op::Jump;
        i.target = target;
        Pos keep = stmt_;
        emit(std::move(i));
        stmt_ = keep;
    }

    void branch(Operand cond, int if_true, int if_false, Pos pos) {
        Instr i;
        i.op = Op::Branch;
        i.a = cond;
        i.target = if_true;
        i.target2 = if_false;
        i.pos = pos;
        emit(std::move(i));
    }

    void copy(int dst, Operand value, Pos pos) {
        Instr i;
        i.op = Op::Copy;
        i.dst = Operand::of_var(dst);
        i.a = value;
        i.pos = pos;
        emit(std::move(i));
    }

    // ------------------------------------------------------------ variables

    int new_var(const std::string& name, ast::Type type, VarKind kind, Pos pos) {
        // Two variables with the same name (in different blocks, or one
        // hiding the other) get different names in the IR: x and x.2.
        int n = ++name_uses_[name];
        return fn_.add_var(n == 1 ? name : name + "." + std::to_string(n), type, kind, pos);
    }

    int new_temp(ast::Type type) { return fn_.add_var("t" + std::to_string(++temps_), type, VarKind::Temp); }

    int lookup(const std::string& name) const {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            auto it = scope->find(name);
            if (it != scope->end()) return it->second;
        }
        return -1;  // can't happen in a program that passed the semantic checks
    }

    sem::Type type_of(const Expr& e) const { return types_.types.at(&e); }

    // ------------------------------------------------------------ functions

    void build_function(const ast::Function& source) {
        fn_ = Function{};
        fn_.name = source.name;
        fn_.pos = source.name_pos;
        fn_.return_type = source.return_type;
        pool_.clear();
        order_.clear();
        cur_ = -1;
        scopes_.assign(1, {});
        loops_.clear();
        name_uses_.clear();
        temps_ = labels_ = nesting_ = 0;
        stmt_ = source.pos;

        for (const auto& p : source.params) {
            int id = new_var(p.name, p.type, VarKind::Param, p.pos);
            fn_.params.push_back(id);
            scopes_.back()[p.name] = id;
        }
        start(create("entry"));
        if (source.body) statements(*source.body);

        // Running off the end returns. For a function with a return type the
        // semantic checks report this path as a missing return.
        if (open()) {
            Instr ret;
            ret.op = Op::Return;
            if (source.return_type.base != ast::BaseType::Void || source.return_type.is_array)
                ret.a = default_value(source.return_type);
            stmt_ = {};
            emit(std::move(ret));
        }
        finish();
    }

    // Puts the blocks in the order they were started and renumbers the jumps.
    void finish() {
        std::vector<int> position(pool_.size(), -1);
        for (size_t i = 0; i < order_.size(); ++i) position[static_cast<size_t>(order_[i])] = static_cast<int>(i);
        for (int id : order_) fn_.blocks.push_back(std::move(pool_[static_cast<size_t>(id)]));
        for (auto& block : fn_.blocks) {
            for (auto& instr : block.instrs) {
                if (instr.target >= 0) instr.target = position[static_cast<size_t>(instr.target)];
                if (instr.target2 >= 0) instr.target2 = position[static_cast<size_t>(instr.target2)];
            }
        }
        link(fn_);
        out_.functions.push_back(std::move(fn_));
    }

    // ------------------------------------------------------------ statements

    void statements(const ast::Block& block) {
        for (const auto& stmt : block.stmts) statement(*stmt);
    }

    void scoped(const ast::Block& block) {
        scopes_.emplace_back();
        statements(block);
        scopes_.pop_back();
    }

    void nested(const ast::Block& block, bool own_scope) {
        fn_.max_nesting = std::max(fn_.max_nesting, ++nesting_);
        if (own_scope) scoped(block);
        else statements(block);
        --nesting_;
    }

    void statement(const Stmt& stmt) {
        stmt_ = stmt.pos;
        switch (stmt.kind) {
            case StmtKind::Block: scoped(ast::as<ast::Block>(stmt)); break;
            case StmtKind::Let: {
                const auto& let = ast::as<ast::Let>(stmt);
                // The variable exists only after its initial value is
                // computed, so "let x: int = x + 1;" reads an outer x.
                int var = new_var(let.name, let.type, VarKind::Local, let.name_pos);
                if (let.init) {
                    assign(var, sem::from_ast(let.type), *let.init, true);
                } else {
                    fn_.vars[static_cast<size_t>(var)].declared_without_value = true;
                    copy(var, default_value(let.type), {});
                }
                scopes_.back()[let.name] = var;
                break;
            }
            case StmtKind::Assign: {
                const auto& a = ast::as<ast::Assign>(stmt);
                if (a.target->kind == ExprKind::Name) {
                    int var = lookup(ast::as<ast::Name>(*a.target).name);
                    assign(var, sem::from_ast(fn_.vars[static_cast<size_t>(var)].type), *a.value, false);
                } else {
                    const auto& ix = ast::as<ast::Index>(*a.target);
                    Instr store;
                    store.op = Op::Store;
                    store.a = expr(*ix.array);
                    store.b = expr(*ix.index);
                    store.c = converted(*a.value, type_of(*a.target));
                    store.pos = ix.pos;
                    store.expr = a.target.get();
                    emit(std::move(store));
                }
                break;
            }
            case StmtKind::ExprStmt: expr(*ast::as<ast::ExprStmt>(stmt).expr); break;
            case StmtKind::If: {
                const auto& s = ast::as<ast::If>(stmt);
                int then_block = create("if.then");
                int else_block = s.else_branch ? create("if.else") : -1;
                int end = create("if.end");
                condition(*s.cond, then_block, s.else_branch ? else_block : end);
                start(then_block);
                nested(*s.then_block, true);
                if (s.else_branch) {
                    if (open()) jump(end);
                    start(else_block);
                    fn_.max_nesting = std::max(fn_.max_nesting, ++nesting_);
                    statement(*s.else_branch);
                    --nesting_;
                }
                start(end);
                break;
            }
            case StmtKind::While: {
                const auto& s = ast::as<ast::While>(stmt);
                int head = create("while.head");
                int body = create("while.body");
                int end = create("while.end");
                start(head);
                condition(*s.cond, body, end);
                start(body);
                loops_.push_back({head, end});
                nested(*s.body, true);
                loops_.pop_back();
                if (open()) jump(head);
                start(end);
                break;
            }
            case StmtKind::For: for_loop(ast::as<ast::For>(stmt)); break;
            case StmtKind::Return: {
                const auto& r = ast::as<ast::Return>(stmt);
                Instr ret;
                ret.op = Op::Return;
                if (r.value) ret.a = converted(*r.value, sem::from_ast(fn_.return_type));
                ret.pos = r.pos;
                emit(std::move(ret));
                break;
            }
            case StmtKind::Break: {
                int target = loops_.back().break_to;
                emit_jump_at(target, stmt.pos);
                break;
            }
            case StmtKind::Continue: {
                int target = loops_.back().continue_to;
                emit_jump_at(target, stmt.pos);
                break;
            }
        }
    }

    void emit_jump_at(int target, Pos pos) {
        Instr i;
        i.op = Op::Jump;
        i.target = target;
        i.pos = pos;
        emit(std::move(i));
    }

    // var = value, computing straight into var when one instruction can do it.
    void assign(int var, sem::Type target, const Expr& value, bool decl_init) {
        bool promote = target == sem::kFloat && type_of(value) == sem::kInt;
        Operand result = promote ? to_float(expr(value)) : expr(value, var);
        if (!result.is_var(var)) copy(var, result, ast::start_of(value));
        if (decl_init && cur_ >= 0 && !pool_[static_cast<size_t>(cur_)].instrs.empty()) {
            Instr& last = pool_[static_cast<size_t>(cur_)].instrs.back();
            if (last.dst.is_var(var)) last.decl_init = true;
        }
    }

    // for i in a..b:
    //         i = a; end = b
    //   head: if i < end goto body else exit
    //   body: ...
    //   step: i = i + 1; goto head
    //   exit:
    // A parallel loop adds "parfor" before the head, where the threads start,
    // and "parend" on the way out, where they stop and are combined.
    void for_loop(const ast::For& loop) {
        scopes_.emplace_back();
        ast::Type int_type{ast::BaseType::Int, false};
        int i = new_var(loop.var, int_type, VarKind::Local, loop.var_pos);
        assign(i, sem::kInt, *loop.start, false);
        int end = new_var(loop.var + ".end", int_type, VarKind::Temp, loop.pos);
        assign(end, sem::kInt, *loop.end, false);
        scopes_.back()[loop.var] = i;

        int head = create("for.head");
        int body = create("for.body");
        int step = create("for.step");
        int exit = create("for.end");
        bool is_parallel = parallel_.count(&loop) > 0;
        int par_end = -1;
        std::vector<Reduction> reductions;
        if (is_parallel) {
            for (const auto& name : loop.reduce_vars)
                reductions.push_back({lookup(name), loop.reduce_op == Kind::Star ? '*' : '+'});
            Instr fork;
            fork.op = Op::ParFor;
            fork.a = Operand::of_var(i);
            fork.b = Operand::of_var(end);
            fork.reductions = reductions;
            par_end = create("par.end");
            fork.target = head;
            fork.target2 = par_end;
            fork.pos = loop.pos;
            emit(std::move(fork));
        }

        start(head);
        Instr test;
        test.op = Op::Lt;
        test.type = VT::Int;
        int t = new_temp(ast::Type{ast::BaseType::Bool, false});
        test.dst = Operand::of_var(t);
        test.a = Operand::of_var(i);
        test.b = Operand::of_var(end);
        test.pos = loop.var_pos;
        emit(std::move(test));
        branch(Operand::of_var(t), body, is_parallel ? par_end : exit, loop.var_pos);

        start(body);
        loops_.push_back({step, exit});
        nested(*loop.body, false);
        loops_.pop_back();

        start(step);
        stmt_ = loop.pos;
        Instr next;
        next.op = Op::Add;
        next.type = VT::Int;
        next.dst = Operand::of_var(i);
        next.a = Operand::of_var(i);
        next.b = Operand::of_int(1);
        emit(std::move(next));  // no position: the compiler added it
        jump(head);

        if (is_parallel) {
            start(par_end);
            Instr join;
            join.op = Op::ParEnd;
            join.reductions = reductions;
            join.pos = loop.pos;
            emit(std::move(join));
        }
        start(exit);
        scopes_.pop_back();
    }

    // ------------------------------------------------------------ conditions (jumping code)

    // Jumps to if_true or if_false without ever storing the condition:
    //   a && b  jumps to if_false as soon as a is false, and only then tests b
    //   a || b  jumps to if_true as soon as a is true
    //   !a      swaps the two targets
    void condition(const Expr& e, int if_true, int if_false) {
        if (e.kind == ExprKind::Binary) {
            const auto& b = ast::as<ast::Binary>(e);
            if (b.op == Kind::And || b.op == Kind::Or) {
                int rhs = create(b.op == Kind::And ? "and.rhs" : "or.rhs");
                if (b.op == Kind::And) condition(*b.lhs, rhs, if_false);
                else condition(*b.lhs, if_true, rhs);
                start(rhs);
                condition(*b.rhs, if_true, if_false);
                return;
            }
        }
        if (e.kind == ExprKind::Unary && ast::as<ast::Unary>(e).op == Kind::Not) {
            condition(*ast::as<ast::Unary>(e).operand, if_false, if_true);
            return;
        }
        if (e.kind == ExprKind::BoolLit) {
            emit_jump_at(ast::as<ast::BoolLit>(e).value ? if_true : if_false, e.pos);
            return;
        }
        branch(expr(e), if_true, if_false, ast::start_of(e));
    }

    // ------------------------------------------------------------ expressions

    Operand to_float(Operand o) {
        if (o.is_int()) return Operand::of_float(static_cast<double>(o.i));
        Instr i;
        i.op = Op::IntToFloat;
        i.type = VT::Float;
        i.dst = Operand::of_var(new_temp(ast::Type{ast::BaseType::Float, false}));
        i.a = o;
        return emit(std::move(i)).dst;
    }

    // The value of e, converted to float when the place it goes needs one.
    Operand converted(const Expr& e, sem::Type target) {
        Operand o = expr(e);
        return target == sem::kFloat && type_of(e) == sem::kInt ? to_float(o) : o;
    }

    // Emits the instruction that computes e, into `want` if given, else a new temporary.
    Operand compute(Instr instr, const Expr& e, int want) {
        int dst = want >= 0 ? want : new_temp(to_ast_type(type_of(e)));
        instr.dst = Operand::of_var(dst);
        instr.expr = &e;
        return emit(std::move(instr)).dst;
    }

    // Returns where the value of e is: a constant, a variable, or a temporary
    // that the emitted code fills. Post-order: operands first, then the operation.
    Operand expr(const Expr& e, int want = -1) {
        switch (e.kind) {
            case ExprKind::IntLit: return Operand::of_int(ast::as<ast::IntLit>(e).value);
            case ExprKind::FloatLit: return Operand::of_float(ast::as<ast::FloatLit>(e).value);
            case ExprKind::BoolLit: return Operand::of_bool(ast::as<ast::BoolLit>(e).value);
            case ExprKind::NullLit: return Operand::null();
            case ExprKind::StringLit:
                out_.strings.push_back(ast::as<ast::StringLit>(e).value);
                return Operand::str(isize(out_.strings) - 1);
            case ExprKind::Name: return Operand::of_var(lookup(ast::as<ast::Name>(e).name));
            case ExprKind::Unary: {
                const auto& u = ast::as<ast::Unary>(e);
                if (u.op == Kind::Minus && u.operand->kind == ExprKind::IntLit)
                    return Operand::of_int(-ast::as<ast::IntLit>(*u.operand).value);
                if (u.op == Kind::Minus && u.operand->kind == ExprKind::FloatLit)
                    return Operand::of_float(-ast::as<ast::FloatLit>(*u.operand).value);
                Instr i;
                i.op = u.op == Kind::Minus ? Op::Neg : Op::Not;
                i.type = value_type(type_of(e));
                i.a = expr(*u.operand);
                i.pos = u.pos;
                return compute(std::move(i), e, want);
            }
            case ExprKind::Binary: return binary(ast::as<ast::Binary>(e), want);
            case ExprKind::Index: {
                const auto& ix = ast::as<ast::Index>(e);
                Instr i;
                i.op = Op::Load;
                i.a = expr(*ix.array);
                i.b = expr(*ix.index);
                i.type = value_type(type_of(e));
                i.pos = ix.pos;
                return compute(std::move(i), e, want);
            }
            case ExprKind::Call: return call(ast::as<ast::Call>(e), want);
            case ExprKind::NewArray: {
                const auto& n = ast::as<ast::NewArray>(e);
                Instr i;
                i.op = Op::NewArray;
                i.elem = n.elem;
                i.a = expr(*n.size);
                i.pos = n.pos;
                return compute(std::move(i), e, want);
            }
        }
        return {};
    }

    Operand binary(const ast::Binary& b, int want) {
        if (b.op == Kind::And || b.op == Kind::Or) {
            // As a value, a short-circuit expression is the condition's jumps
            // ending in "result = true" or "result = false".
            int result = new_temp(ast::Type{ast::BaseType::Bool, false});
            int if_true = create("bool.true");
            int if_false = create("bool.false");
            int end = create("bool.end");
            condition(b, if_true, if_false);
            start(if_true);
            copy(result, Operand::of_bool(true), b.pos);
            jump(end);
            start(if_false);
            copy(result, Operand::of_bool(false), b.pos);
            start(end);
            return Operand::of_var(result);
        }

        sem::Type lt = type_of(*b.lhs);
        sem::Type rt = type_of(*b.rhs);
        Operand l = expr(*b.lhs);
        Operand r = expr(*b.rhs);
        Instr i;
        i.op = binary_op(b.op);
        if (lt == sem::kFloat || rt == sem::kFloat) {
            // int is promoted to float when the other side is a float.
            if (lt == sem::kInt) l = to_float(l);
            if (rt == sem::kInt) r = to_float(r);
            i.type = VT::Float;
        } else {
            i.type = value_type(lt == sem::kNull ? rt : lt);
        }
        i.a = l;
        i.b = r;
        i.pos = b.pos;
        return compute(std::move(i), b, want);
    }

    Operand call(const ast::Call& c, int want) {
        Instr i;
        i.pos = c.pos;
        i.expr = &c;
        if (c.callee == "print") {
            i.op = Op::Print;
            for (const auto& arg : c.args) i.args.push_back(expr(*arg));
            emit(std::move(i));
            return {};
        }
        if (c.callee == "len") {
            i.op = Op::Len;
            i.a = expr(*c.args[0]);
            return compute(std::move(i), c, want);
        }
        const ast::Function& callee = *signatures_.at(c.callee);
        i.op = Op::Call;
        i.callee = c.callee;
        for (size_t k = 0; k < c.args.size(); ++k)
            i.args.push_back(converted(*c.args[k], sem::from_ast(callee.params[k].type)));
        if (type_of(c) == sem::kVoid) {
            emit(std::move(i));
            return {};
        }
        return compute(std::move(i), c, want);
    }
};

}  // namespace

Program build(const ast::Program& program, const sem::Result& types,
              const std::unordered_set<const ast::For*>& parallel) {
    return Builder(program, types, parallel).run();
}

}  // namespace parc::ir
