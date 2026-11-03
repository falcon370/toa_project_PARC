#include "vm/vm.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <limits>
#include <unordered_map>

#include "common/json.hpp"
#include "common/json_writer.hpp"

namespace parc::vm {

std::string to_string(const Value& v) {
    switch (v.tag) {
        case Value::Tag::Int: return std::to_string(v.i);
        case Value::Tag::Float: {
            char buf[48];
            std::snprintf(buf, sizeof buf, "%g", v.f);
            return buf;
        }
        case Value::Tag::Bool: return v.i ? "true" : "false";
        case Value::Tag::Ref: return v.i == 0 ? "null" : "array#" + std::to_string(v.i);
    }
    return "?";
}

namespace {

constexpr size_t kMaxDepth = 2000;
constexpr long long kMaxCells = 16'000'000;
constexpr size_t kMaxOutput = 1 << 20;
constexpr size_t kMaxEvents = 20;
constexpr size_t kMaxSchedule = 400;
constexpr long long kIntMin = std::numeric_limits<std::int32_t>::min();
constexpr long long kIntMax = std::numeric_limits<std::int32_t>::max();

struct Array {
    int elem = 0;
    std::vector<Value> data;
};

struct Frame {
    int fn = 0;
    int pc = 0;
    std::vector<Value> regs;
    std::vector<bool> written;    // which registers hold a value, for the debugger
    std::vector<Value> slots;
    std::vector<Value> args;      // the arguments this call received
    std::vector<Value> out_args;  // arguments collected for the next call
    int ret_reg = -1;             // where the caller wants the result
};

struct Thread {
    enum class State { Free, Ready, Waiting, Done };
    State state = State::Free;
    std::vector<Frame> frames;
    int parent = -1;
    std::vector<int> children;
    int live_children = 0;
    const ParLoop* loop = nullptr;  // the parallel loop this thread is a worker of
    int worker = 0;                 // 0 = the main thread; workers are 1, 2, ...
};

struct Trap {
    std::string message;
};

// Exact text for comparing two runs: floats with all their digits.
std::string exact(const Value& v) {
    if (v.tag != Value::Tag::Float) return to_string(v);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.17g", v.f);
    return buf;
}

class Machine {
public:
    Machine(const Program& program, const Options& options) : program_(program), options_(options) {
        rng_ = options.seed ? options.seed : 1;
        function_steps_.assign(program.functions.size(), 0);
    }

    Result run(int function, const std::vector<Arg>& args) {
        std::vector<int> arg_arrays;
        Frame frame = new_frame(function);
        for (const Arg& arg : args) {
            if (!arg.is_array) {
                frame.args.push_back(arg.value);
            } else if (arg.is_null) {
                frame.args.push_back(Value::of_ref(0));
            } else {
                heap_.push_back({arg.elem, arg.items});
                frame.args.push_back(Value::of_ref(static_cast<std::int32_t>(heap_.size())));
                arg_arrays.push_back(static_cast<int>(heap_.size()));
            }
        }
        threads_.emplace_back();
        threads_[0].state = Thread::State::Ready;
        threads_[0].frames.push_back(std::move(frame));

        try {
            int current = 0;
            while (threads_[0].state != Thread::State::Done) {
                run_thread(current);
                if (threads_[0].state == Thread::State::Done) break;
                current = pick_next();
            }
        } catch (const Trap& trap) {
            result_.ok = false;
            result_.error = trap.message;
            result_.error_line = line_;
            result_.error_function = program_.functions[static_cast<size_t>(function_)].name;
        }
        result_.steps = steps_;
        result_.function_steps = std::move(function_steps_);
        result_.calls = calls_;
        result_.max_depth = max_depth_;
        result_.heap_cells = cells_;
        result_.heap_arrays = static_cast<long long>(heap_.size());
        for (int id : arg_arrays) result_.arrays.push_back(array_text(id));
        return std::move(result_);
    }

private:
    const Program& program_;
    Options options_;
    Result result_;
    std::deque<Thread> threads_;  // a deque: threads are added while others are running
    std::vector<Array> heap_;
    long long cells_ = 0;
    long long steps_ = 0;
    std::vector<long long> function_steps_;
    long long calls_ = 0;
    int max_depth_ = 1;
    std::uint32_t rng_ = 1;
    int line_ = 0;      // of the instruction being executed, for error messages
    int function_ = 0;

    // For the race detector: who touched each array element in the current parallel loop.
    struct Touch {
        int writer = 0;   // worker number, 0 = nobody
        int reader = 0;   // the first two different threads that read it
        int reader2 = 0;
        bool reported = false;
    };
    std::unordered_map<std::uint64_t, Touch> touched_;

    // ------------------------------------------------------------ helpers

    Frame new_frame(int function) const {
        const Function& fn = program_.functions[static_cast<size_t>(function)];
        Frame frame;
        frame.fn = function;
        frame.regs.assign(static_cast<size_t>(program_.registers), Value{});
        frame.written.assign(static_cast<size_t>(program_.registers), false);
        frame.slots.assign(static_cast<size_t>(fn.slots), Value{});
        return frame;
    }

    [[noreturn]] static void trap(std::string message) { throw Trap{std::move(message)}; }

    std::string array_text(int id) const {
        const Array& a = heap_[static_cast<size_t>(id) - 1];
        std::string out = "[";
        for (size_t i = 0; i < a.data.size(); ++i) out += (i ? ", " : "") + exact(a.data[i]);
        return out + "]";
    }

    void event(const char* kind, std::string text) {
        if (result_.events.size() >= kMaxEvents) return;
        result_.events.push_back({kind, line_, program_.functions[static_cast<size_t>(function_)].name, std::move(text)});
    }

    // An int result computed without limits, stored the way a 32-bit machine would.
    Value wrap(long long wide, const char* what, long long a, long long b) {
        if (wide < kIntMin || wide > kIntMax) {
            ++result_.overflows;
            auto stored = static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(wide)));
            event("overflow", std::to_string(a) + " " + what + " " + std::to_string(b) + " does not fit in an int: it gives " +
                                  std::to_string(wide) + ", which wrapped around to " + std::to_string(stored));
            return Value::of_int(stored);
        }
        return Value::of_int(static_cast<std::int32_t>(wide));
    }

    std::uint32_t random() {  // xorshift: small, fast, and the same on every machine
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return rng_;
    }

    static Value& at(Frame& frame, Loc loc) {
        return loc.slot ? frame.slots[static_cast<size_t>(loc.index)] : frame.regs[static_cast<size_t>(loc.index)];
    }

    static void store(Frame& frame, Loc loc, Value v) {
        at(frame, loc) = v;
        if (!loc.slot) frame.written[static_cast<size_t>(loc.index)] = true;
    }

    Array& array(const Value& ref) {
        if (ref.i == 0) trap("the array is null");
        return heap_[static_cast<size_t>(ref.i) - 1];
    }

    void check_index(const Array& a, std::int32_t index) {
        if (index < 0 || static_cast<size_t>(index) >= a.data.size())
            trap("index " + std::to_string(index) + " is outside the array (it has " + std::to_string(a.data.size()) +
                 (a.data.size() == 1 ? " element)" : " elements)"));
    }

    // Two threads touching one element, at least one of them writing, is a data race.
    void touch(const Thread& t, std::int32_t ref, std::int32_t index, bool write) {
        if (t.worker == 0) return;
        Touch& e = touched_[(static_cast<std::uint64_t>(static_cast<std::uint32_t>(ref)) << 32) |
                            static_cast<std::uint32_t>(index)];
        int other = 0;
        if (e.writer != 0 && e.writer != t.worker) other = e.writer;
        if (write && other == 0) {
            if (e.reader != 0 && e.reader != t.worker) other = e.reader;
            else if (e.reader2 != 0) other = e.reader2;
        }
        if (other != 0 && !e.reported) {
            e.reported = true;
            ++result_.races;
            event("race", "data race on element " + std::to_string(index) + " of an array: thread " +
                              std::to_string(t.worker) + (write ? " writes it" : " reads it") + " and thread " +
                              std::to_string(other) + " also uses it, so the result depends on which runs first");
        }
        if (write) {
            e.writer = t.worker;
        } else if (e.reader == 0) {
            e.reader = t.worker;
        } else if (e.reader != t.worker && e.reader2 == 0) {
            e.reader2 = t.worker;
        }
    }

    void print(std::string text) {
        if (result_.output.size() + text.size() > kMaxOutput) trap("the program printed more than 1 MB of text");
        result_.output += text;
        printed_ += text;
    }
    std::string printed_;  // by the current instruction, for the trace
    std::string heap_note_;

    // ------------------------------------------------------------ threads

    int pick_next() {
        std::vector<int> ready;
        for (size_t i = 0; i < threads_.size(); ++i)
            if (threads_[i].state == Thread::State::Ready) ready.push_back(static_cast<int>(i));
        if (ready.empty()) trap("no thread can run (this is a bug in the VM)");
        int chosen = ready.size() == 1 ? ready[0] : ready[random() % ready.size()];
        int worker = threads_[static_cast<size_t>(chosen)].worker;
        if (worker != 0) {
            if (!result_.schedule.empty() && result_.schedule.back().first == worker) ++result_.schedule.back().second;
            else if (result_.schedule.size() < kMaxSchedule) result_.schedule.push_back({worker, 1});
        }
        return chosen;
    }

    int new_thread() {
        for (size_t i = 1; i < threads_.size(); ++i)
            if (threads_[i].state == Thread::State::Free) return static_cast<int>(i);
        threads_.emplace_back();
        return static_cast<int>(threads_.size()) - 1;
    }

    static Value identity(const Reduction& r) {
        if (r.is_float) return Value::of_float(r.op == '*' ? 1.0 : 0.0);
        return Value::of_int(r.op == '*' ? 1 : 0);
    }

    Value combine(const Reduction& r, const Value& a, const Value& b) {
        if (r.is_float) return Value::of_float(r.op == '*' ? a.f * b.f : a.f + b.f);
        long long wide = r.op == '*' ? static_cast<long long>(a.i) * b.i : static_cast<long long>(a.i) + b.i;
        return wrap(wide, r.op == '*' ? "*" : "+", a.i, b.i);
    }

    // PARFOR: split the range a..b into one part per thread and start the threads.
    void fork(int id, const Instr& instr) {
        const ParLoop& loop = program_.functions[static_cast<size_t>(threads_[static_cast<size_t>(id)].frames.back().fn)]
                                  .loops[static_cast<size_t>(instr.imm)];
        Frame& frame = threads_[static_cast<size_t>(id)].frames.back();
        long long lo = at(frame, loop.var).i;
        long long hi = at(frame, loop.end).i;
        long long count = hi - lo;
        if (count <= 0) {  // nothing to do: continue after the loop
            frame.pc = loop.end_pc + 1;
            return;
        }
        bool nested = threads_[static_cast<size_t>(id)].worker != 0;
        // A parallel loop inside a parallel loop does not start more threads.
        long long workers = nested ? 1 : std::min<long long>(std::max(1, options_.threads), count);
        if (!nested) {
            touched_.clear();
            ++result_.parallel_loops;
        }
        Frame copy = frame;  // every thread starts with its own copy of all local variables
        copy.out_args.clear();
        for (long long k = 0; k < workers; ++k) {
            int child = new_thread();
            Thread& t = threads_[static_cast<size_t>(child)];
            t = Thread{};
            t.state = Thread::State::Ready;
            t.parent = id;
            t.loop = &loop;
            t.worker = nested ? threads_[static_cast<size_t>(id)].worker : static_cast<int>(k) + 1;
            t.frames.push_back(copy);
            Frame& own = t.frames.back();
            store(own, loop.var, Value::of_int(static_cast<std::int32_t>(lo + k * count / workers)));
            store(own, loop.end, Value::of_int(static_cast<std::int32_t>(lo + (k + 1) * count / workers)));
            for (const auto& r : loop.reductions) store(own, r.loc, identity(r));
            threads_[static_cast<size_t>(id)].children.push_back(child);
        }
        Thread& parent = threads_[static_cast<size_t>(id)];
        parent.live_children = static_cast<int>(workers);
        parent.state = Thread::State::Waiting;
    }

    // All threads of a loop reached PAREND: combine their results and wake the parent.
    void join(int id) {
        Thread& parent = threads_[static_cast<size_t>(id)];
        const ParLoop& loop = *threads_[static_cast<size_t>(parent.children[0])].loop;
        Frame& frame = parent.frames.back();
        for (const auto& r : loop.reductions) {
            // Tree reduction: combine neighbours pairwise until one value is left.
            std::vector<Value> values;
            for (int child : parent.children) values.push_back(at(threads_[static_cast<size_t>(child)].frames[0], r.loc));
            while (values.size() > 1) {
                std::vector<Value> next;
                for (size_t i = 0; i + 1 < values.size(); i += 2) next.push_back(combine(r, values[i], values[i + 1]));
                if (values.size() % 2 == 1) next.push_back(values.back());
                values = std::move(next);
            }
            store(frame, r.loc, combine(r, at(frame, r.loc), values[0]));
        }
        store(frame, loop.var, at(frame, loop.end));  // as after a serial loop
        frame.pc = loop.end_pc + 1;
        for (int child : parent.children) threads_[static_cast<size_t>(child)] = Thread{};
        parent.children.clear();
        parent.state = Thread::State::Ready;
    }

    // ------------------------------------------------------------ the interpreter loop

    // Runs one thread until it waits, finishes, or completes a pass of its parallel loop.
    void run_thread(int id) {
        while (true) {
            Thread& t = threads_[static_cast<size_t>(id)];
            if (t.state != Thread::State::Ready) return;
            step(id);
            Thread& now = threads_[static_cast<size_t>(id)];
            if (now.state != Thread::State::Ready) return;
            if (now.loop && now.frames.size() == 1 && now.frames[0].pc == now.loop->head_pc) return;  // let another thread run
        }
    }

    void record(int id, int function, int pc, int line) {
        if (static_cast<int>(result_.trace.size()) >= options_.trace_steps) return;
        const Thread& t = threads_[static_cast<size_t>(id)];
        TraceStep s;
        s.thread = t.worker;
        s.function = function;
        s.pc = pc;
        s.line = line;
        s.depth = static_cast<int>(t.frames.size());
        if (!t.frames.empty()) {
            const Frame& frame = t.frames.back();
            for (size_t r = 0; r < frame.regs.size(); ++r)
                s.regs.push_back(frame.written[r] ? display(frame.regs[r]) : "");
            for (const auto& v : frame.slots) s.slots.push_back(display(v));
        }
        s.output = printed_;
        s.heap = heap_note_;
        result_.trace.push_back(std::move(s));
    }

    static std::string display(const Value& v) {
        if (v.tag == Value::Tag::Ref) return v.i == 0 ? "null" : "#" + std::to_string(v.i);
        return to_string(v);
    }

    void step(int id) {
        Thread& t = threads_[static_cast<size_t>(id)];
        Frame& f = t.frames.back();
        const Function& fn = program_.functions[static_cast<size_t>(f.fn)];
        const Instr& in = fn.code[static_cast<size_t>(f.pc)];
        int pc = f.pc;
        int function = f.fn;
        line_ = in.line;
        function_ = f.fn;
        if (++steps_ > options_.max_steps)
            trap("the program ran for more than " + std::to_string(options_.max_steps) +
                 " steps and was stopped (an endless loop?)");
        ++function_steps_[static_cast<size_t>(f.fn)];
        printed_.clear();
        heap_note_.clear();

        auto reg = [&f](int r) -> const Value& { return f.regs[static_cast<size_t>(r)]; };
        auto set = [&f](int r, Value v) {
            f.regs[static_cast<size_t>(r)] = v;
            f.written[static_cast<size_t>(r)] = true;
        };
        int next = f.pc + 1;
        bool frame_changed = false;

        switch (in.op) {
            case VOp::LI: set(in.a, Value::of_int(static_cast<std::int32_t>(in.imm))); break;
            case VOp::LF: set(in.a, Value::of_float(in.fimm)); break;
            case VOp::LB: set(in.a, Value::of_bool(in.imm != 0)); break;
            case VOp::LNULL: set(in.a, Value::of_ref(0)); break;
            case VOp::MOV: set(in.a, reg(in.b)); break;

            case VOp::ADD: set(in.a, wrap(static_cast<long long>(reg(in.b).i) + reg(in.c).i, "+", reg(in.b).i, reg(in.c).i)); break;
            case VOp::SUB: set(in.a, wrap(static_cast<long long>(reg(in.b).i) - reg(in.c).i, "-", reg(in.b).i, reg(in.c).i)); break;
            case VOp::MUL: set(in.a, wrap(static_cast<long long>(reg(in.b).i) * reg(in.c).i, "*", reg(in.b).i, reg(in.c).i)); break;
            case VOp::ADDI: set(in.a, wrap(static_cast<long long>(reg(in.b).i) + in.imm, "+", reg(in.b).i, in.imm)); break;
            case VOp::SUBI: set(in.a, wrap(static_cast<long long>(reg(in.b).i) - in.imm, "-", reg(in.b).i, in.imm)); break;
            case VOp::MULI: set(in.a, wrap(static_cast<long long>(reg(in.b).i) * in.imm, "*", reg(in.b).i, in.imm)); break;
            case VOp::NEG: set(in.a, wrap(-static_cast<long long>(reg(in.b).i), "*", reg(in.b).i, -1)); break;
            case VOp::DIV:
            case VOp::MOD: {
                long long a = reg(in.b).i;
                long long b = reg(in.c).i;
                if (b == 0) trap(in.op == VOp::DIV ? "division by zero" : "remainder of a division by zero");
                if (in.op == VOp::DIV) set(in.a, wrap(a / b, "/", a, b));  // only -2147483648 / -1 can overflow
                else set(in.a, Value::of_int(b == -1 ? 0 : static_cast<std::int32_t>(a % b)));
                break;
            }

            case VOp::FADD: set(in.a, Value::of_float(reg(in.b).f + reg(in.c).f)); break;
            case VOp::FSUB: set(in.a, Value::of_float(reg(in.b).f - reg(in.c).f)); break;
            case VOp::FMUL: set(in.a, Value::of_float(reg(in.b).f * reg(in.c).f)); break;
            case VOp::FDIV: set(in.a, Value::of_float(reg(in.b).f / reg(in.c).f)); break;
            case VOp::FNEG: set(in.a, Value::of_float(-reg(in.b).f)); break;

            case VOp::LT: set(in.a, Value::of_bool(reg(in.b).i < reg(in.c).i)); break;
            case VOp::LE: set(in.a, Value::of_bool(reg(in.b).i <= reg(in.c).i)); break;
            case VOp::GT: set(in.a, Value::of_bool(reg(in.b).i > reg(in.c).i)); break;
            case VOp::GE: set(in.a, Value::of_bool(reg(in.b).i >= reg(in.c).i)); break;
            case VOp::LTI: set(in.a, Value::of_bool(reg(in.b).i < in.imm)); break;
            case VOp::LEI: set(in.a, Value::of_bool(reg(in.b).i <= in.imm)); break;
            case VOp::GTI: set(in.a, Value::of_bool(reg(in.b).i > in.imm)); break;
            case VOp::GEI: set(in.a, Value::of_bool(reg(in.b).i >= in.imm)); break;
            case VOp::EQI: set(in.a, Value::of_bool(reg(in.b).i == in.imm)); break;
            case VOp::NEI: set(in.a, Value::of_bool(reg(in.b).i != in.imm)); break;
            case VOp::FLT: set(in.a, Value::of_bool(reg(in.b).f < reg(in.c).f)); break;
            case VOp::FLE: set(in.a, Value::of_bool(reg(in.b).f <= reg(in.c).f)); break;
            case VOp::FGT: set(in.a, Value::of_bool(reg(in.b).f > reg(in.c).f)); break;
            case VOp::FGE: set(in.a, Value::of_bool(reg(in.b).f >= reg(in.c).f)); break;
            case VOp::EQ:
            case VOp::NE: {
                const Value& a = reg(in.b);
                const Value& b = reg(in.c);
                bool equal = a.tag == Value::Tag::Float ? a.f == b.f : a.i == b.i;
                set(in.a, Value::of_bool(equal == (in.op == VOp::EQ)));
                break;
            }
            case VOp::NOT: set(in.a, Value::of_bool(reg(in.b).i == 0)); break;
            case VOp::I2F: set(in.a, Value::of_float(static_cast<double>(reg(in.b).i))); break;

            case VOp::NEWARR: {
                long long n = reg(in.b).i;
                if (n < 0) trap("the array size is negative (" + std::to_string(n) + ")");
                cells_ += n;
                if (cells_ > kMaxCells) trap("the program asked for more memory than the VM has (16 million array elements)");
                Value empty = in.imm == 1 ? Value::of_float(0.0) : in.imm == 2 ? Value::of_bool(false) : Value::of_int(0);
                heap_.push_back({static_cast<int>(in.imm), std::vector<Value>(static_cast<size_t>(n), empty)});
                set(in.a, Value::of_ref(static_cast<std::int32_t>(heap_.size())));
                heap_note_ = "new #" + std::to_string(heap_.size()) + " [" + std::to_string(n) + "]";
                break;
            }
            case VOp::ALOAD: {
                Array& a = array(reg(in.b));
                std::int32_t index = reg(in.c).i;
                check_index(a, index);
                touch(t, reg(in.b).i, index, false);
                set(in.a, a.data[static_cast<size_t>(index)]);
                break;
            }
            case VOp::ASTORE: {
                Array& a = array(reg(in.a));
                std::int32_t index = reg(in.b).i;
                check_index(a, index);
                touch(t, reg(in.a).i, index, true);
                a.data[static_cast<size_t>(index)] = reg(in.c);
                heap_note_ = "#" + std::to_string(reg(in.a).i) + "[" + std::to_string(index) + "] = " + display(reg(in.c));
                break;
            }
            case VOp::LEN: set(in.a, Value::of_int(static_cast<std::int32_t>(array(reg(in.b)).data.size()))); break;

            case VOp::JMP: next = static_cast<int>(in.imm); break;
            case VOp::BRT:
                if (reg(in.a).i != 0) next = static_cast<int>(in.imm);
                break;
            case VOp::BRF:
                if (reg(in.a).i == 0) next = static_cast<int>(in.imm);
                break;

            case VOp::ARG: f.out_args.push_back(reg(in.a)); break;
            case VOp::CALL: {
                if (t.frames.size() >= kMaxDepth)
                    trap("the call stack is more than " + std::to_string(kMaxDepth) + " calls deep (endless recursion?)");
                Frame callee = new_frame(static_cast<int>(in.imm));
                callee.args = std::move(f.out_args);
                callee.ret_reg = in.a;
                f.out_args.clear();
                f.pc = next;  // where to continue after the call returns
                t.frames.push_back(std::move(callee));
                ++calls_;
                max_depth_ = std::max(max_depth_, static_cast<int>(t.frames.size()));
                frame_changed = true;
                break;
            }
            case VOp::RET: {
                Value value = in.a >= 0 ? reg(in.a) : Value{};
                bool has_value = in.a >= 0;
                int ret_reg = f.ret_reg;
                t.frames.pop_back();
                frame_changed = true;
                if (t.frames.empty()) {
                    t.state = Thread::State::Done;
                    if (has_value) result_.returned = value.tag == Value::Tag::Ref && value.i != 0 ? array_text(value.i) : exact(value);
                } else if (has_value && ret_reg >= 0) {
                    Frame& caller = t.frames.back();
                    caller.regs[static_cast<size_t>(ret_reg)] = value;
                    caller.written[static_cast<size_t>(ret_reg)] = true;
                }
                break;
            }
            case VOp::GETARG: set(in.a, f.args[static_cast<size_t>(in.imm)]); break;
            case VOp::LOADS: set(in.a, f.slots[static_cast<size_t>(in.imm)]); break;
            case VOp::STORES: f.slots[static_cast<size_t>(in.imm)] = reg(in.a); break;

            case VOp::PRINT: print(to_string(reg(in.a)) + (in.c == 1 ? "\n" : " ")); break;
            case VOp::PRINTS: print(program_.strings[static_cast<size_t>(in.imm)] + (in.c == 1 ? "\n" : " ")); break;
            case VOp::PRINTLN: print("\n"); break;

            case VOp::PARFOR:
                f.pc = next;  // forked threads start here; fork() moves the parent past the loop if it is empty
                fork(id, in);
                frame_changed = true;
                break;
            case VOp::PAREND: {
                if (!t.loop) break;  // only threads of the loop stop here
                t.state = Thread::State::Done;
                int parent = t.parent;
                frame_changed = true;
                record(id, function, pc, in.line);
                if (--threads_[static_cast<size_t>(parent)].live_children == 0) join(parent);
                return;
            }
        }
        if (!frame_changed) f.pc = next;
        record(id, function, pc, in.line);
    }
};

}  // namespace

Result run(const Program& program, const Options& options) {
    int main_fn = program.find("main");
    if (main_fn < 0) {
        Result r;
        r.ok = false;
        r.error = "there is nothing to run: the program has no 'fn main()'";
        return r;
    }
    return Machine(program, options).run(main_fn, {});
}

Result run_function(const Program& program, int function, const std::vector<Arg>& args, const Options& options) {
    return Machine(program, options).run(function, args);
}

std::string to_json(const Result& result, const Program& program) {
    json::Writer w;
    auto strings = [&w](const std::vector<std::string>& items) {
        w.begin_array();
        for (const auto& item : items) w.string(item);
        w.end_array();
    };
    w.begin_object();
    w.key("ok");
    w.boolean(result.ok);
    w.key("output");
    w.string(result.output);
    if (!result.ok) {
        w.key("error");
        w.begin_object();
        w.key("message");
        w.string(result.error);
        w.key("line");
        w.integer(result.error_line);
        w.key("function");
        w.string(result.error_function);
        w.end_object();
    }
    w.key("returned");
    w.string(result.returned);
    w.key("steps");
    w.integer(result.steps);
    w.key("calls");
    w.integer(result.calls);
    w.key("max_depth");
    w.integer(result.max_depth);
    w.key("heap_cells");
    w.integer(result.heap_cells);
    w.key("function_steps");
    w.begin_object();
    for (size_t i = 0; i < result.function_steps.size() && i < program.functions.size(); ++i) {
        w.key(program.functions[i].name);
        w.integer(result.function_steps[i]);
    }
    w.end_object();
    w.key("overflows");
    w.integer(result.overflows);
    w.key("races");
    w.integer(result.races);
    w.key("parallel_loops");
    w.integer(result.parallel_loops);
    w.key("events");
    w.begin_array();
    for (const auto& e : result.events) {
        w.begin_object();
        w.key("kind");
        w.string(e.kind);
        w.key("line");
        w.integer(e.line);
        w.key("function");
        w.string(e.function);
        w.key("text");
        w.string(e.text);
        w.end_object();
    }
    w.end_array();
    w.key("schedule");
    w.begin_array();
    for (const auto& [thread, passes] : result.schedule) {
        w.begin_array();
        w.integer(thread);
        w.integer(passes);
        w.end_array();
    }
    w.end_array();
    w.key("arrays");
    strings(result.arrays);
    w.key("trace");
    w.begin_array();
    for (const auto& s : result.trace) {
        w.begin_object();
        w.key("thread");
        w.integer(s.thread);
        w.key("function");
        w.string(program.functions[static_cast<size_t>(s.function)].name);
        w.key("pc");
        w.integer(s.pc);
        w.key("line");
        w.integer(s.line);
        w.key("depth");
        w.integer(s.depth);
        w.key("regs");
        strings(s.regs);
        w.key("slots");
        strings(s.slots);
        if (!s.output.empty()) {
            w.key("output");
            w.string(s.output);
        }
        if (!s.heap.empty()) {
            w.key("heap");
            w.string(s.heap);
        }
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return w.str() + "\n";
}

}  // namespace parc::vm
