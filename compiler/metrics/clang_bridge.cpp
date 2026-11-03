#include "metrics/clang_bridge.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <tuple>

#include "metrics/measure.hpp"

#ifdef PARC_HAVE_LIBCLANG
#include <clang-c/Index.h>
#endif
#ifdef PARC_HAVE_LLVM_REMARKS
#include <llvm-c/Remarks.h>
#endif

namespace parc::metrics {

namespace {

bool ident_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

std::string parc_names(const std::string& text) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        if (!ident_char(text[i]) || (i > 0 && ident_char(text[i - 1]))) {
            out += text[i++];
            continue;
        }
        size_t end = i;
        while (end < text.size() && ident_char(text[end])) ++end;
        std::string word = text.substr(i, end - i);
        if (word.size() > 2 && (word.rfind("v_", 0) == 0 || word.rfind("f_", 0) == 0)) word = word.substr(2);
        i = end;
        if (text.compare(i, 3, "->n") == 0 && (i + 3 >= text.size() || !ident_char(text[i + 3]))) {
            out += "len(" + word + ")";
            i += 3;
        } else if (text.compare(i, 4, "->d[") == 0) {
            out += word + "[";
            i += 4;
        } else {
            out += word;
        }
    }
    return out;
}

std::string clang_path() {
#ifdef _MSC_VER
    char* env = nullptr;
    size_t length = 0;
    std::string chosen;
    if (_dupenv_s(&env, &length, "PARC_CLANG") == 0 && env) chosen = env;
    std::free(env);
    if (!chosen.empty()) return chosen;
#else
    if (const char* env = std::getenv("PARC_CLANG"); env && *env) return env;
#endif
#ifdef PARC_CLANG_EXE
    if (std::filesystem::exists(PARC_CLANG_EXE)) return PARC_CLANG_EXE;
#endif
    return "clang";
}

// ---------------------------------------------------------------- libclang

#ifdef PARC_HAVE_LIBCLANG

namespace {

std::string take(CXString s) {
    const char* c = clang_getCString(s);
    std::string out = c ? c : "";
    clang_disposeString(s);
    return out;
}

std::vector<CXCursor> children_of(CXCursor cursor) {
    std::vector<CXCursor> out;
    clang_visitChildren(
        cursor,
        [](CXCursor child, CXCursor, CXClientData data) {
            static_cast<std::vector<CXCursor>*>(data)->push_back(child);
            return CXChildVisit_Continue;
        },
        &out);
    return out;
}

// Copies libclang's cursors into CNode trees.
class Converter {
public:
    Converter(const std::string& text, bool parc) : text_(text), parc_(parc) {}

    CNode convert(CXCursor c) {
        CXCursorKind kind = clang_getCursorKind(c);
        std::vector<CXCursor> kids = children_of(c);
        // An implicit conversion: Clang's tree has a node for it, the analysis does not need one.
        if (kind == CXCursor_UnexposedExpr && kids.size() == 1) return convert(kids[0]);

        CNode n;
        n.kind = take(clang_getCursorKindSpelling(kind));
        n.name = take(clang_getCursorSpelling(c));
        n.line = line_of(c);
        n.text = slice(c);
        switch (kind) {
            case CXCursor_BinaryOperator:
            case CXCursor_CompoundAssignOperator:
                n.op = take(clang_getBinaryOperatorKindSpelling(clang_getCursorBinaryOperatorKind(c)));
                break;
            case CXCursor_UnaryOperator:
                n.op = take(clang_getUnaryOperatorKindSpelling(clang_getCursorUnaryOperatorKind(c)));
                break;
            case CXCursor_IntegerLiteral:
                if (CXEvalResult r = clang_Cursor_Evaluate(c)) {
                    if (clang_EvalResult_getKind(r) == CXEval_Int) n.value = clang_EvalResult_getAsLongLong(r);
                    clang_EvalResult_dispose(r);
                }
                break;
            case CXCursor_VarDecl:
            case CXCursor_ParmDecl:
                n.decl = key(c);
                n.type = take(clang_getTypeSpelling(clang_getCursorType(c)));
                break;
            case CXCursor_DeclRefExpr: {
                CXCursor ref = clang_getCursorReferenced(c);
                if (!clang_Cursor_isNull(ref)) {
                    CXCursorKind rk = clang_getCursorKind(ref);
                    n.decl = key(ref);
                    n.param = rk == CXCursor_ParmDecl;
                    n.function = rk == CXCursor_FunctionDecl;
                }
                break;
            }
            case CXCursor_CallExpr: {
                // The children of a call are its arguments; the callee is in `name`.
                int count = clang_Cursor_getNumArguments(c);
                for (int i = 0; i < count; ++i) n.children.push_back(convert(clang_Cursor_getArgument(c, static_cast<unsigned>(i))));
                return n;
            }
            default: break;
        }
        for (CXCursor kid : kids) {
            CXCursorKind k = clang_getCursorKind(kid);
            if (k == CXCursor_TypeRef || k == CXCursor_ParmDecl) continue;
            n.children.push_back(convert(kid));
        }
        return n;
    }

    static int line_of(CXCursor c) {
        CXString file;
        unsigned line = 0;
        unsigned col = 0;
        clang_getPresumedLocation(clang_getCursorLocation(c), &file, &line, &col);
        clang_disposeString(file);
        return static_cast<int>(line);
    }

private:
    const std::string& text_;
    bool parc_;

    // A declaration's id: its offset in the file, plus one so that 0 means none.
    static unsigned key(CXCursor c) {
        CXFile file;
        unsigned line = 0;
        unsigned col = 0;
        unsigned offset = 0;
        clang_getSpellingLocation(clang_getCursorLocation(c), &file, &line, &col, &offset);
        return offset + 1;
    }

    std::string slice(CXCursor c) const {
        CXSourceRange range = clang_getCursorExtent(c);
        CXSourceLocation start = clang_getRangeStart(range);
        if (!clang_Location_isFromMainFile(start)) return {};
        CXFile file;
        unsigned line = 0;
        unsigned col = 0;
        unsigned from = 0;
        unsigned to = 0;
        clang_getSpellingLocation(start, &file, &line, &col, &from);
        clang_getSpellingLocation(clang_getRangeEnd(range), &file, &line, &col, &to);
        if (to <= from || to > text_.size() || to - from > 240) return {};
        std::string out = text_.substr(from, to - from);
        return parc_ ? parc_names(out) : out;
    }
};

std::string presumed_file(CXSourceLocation loc, int* line, int* col) {
    CXString file;
    unsigned l = 0;
    unsigned c = 0;
    clang_getPresumedLocation(loc, &file, &l, &c);
    *line = static_cast<int>(l);
    *col = static_cast<int>(c);
    return take(file);
}

}  // namespace

CUnit parse_c(const std::string& c_text, const std::string& display_file, bool from_parc) {
    CUnit unit;
    unit.parc = from_parc;
    unit.version = take(clang_getClangVersion());
    unit.version = unit.version.substr(0, unit.version.find(" ("));  // "clang version 23.1.2", without the git hash
    // The file libclang reads: the ParC translation is passed from memory.
    std::string file_name = from_parc ? display_file + ".c" : display_file;
    std::vector<const char*> args = {"-x", "c", "-std=c11", "-Wall", "-Wextra", "-fwrapv"};
#ifdef PARC_CLANG_RESOURCE_DIR
    args.push_back("-resource-dir");
    args.push_back(PARC_CLANG_RESOURCE_DIR);
#endif
    CXUnsavedFile unsaved{file_name.c_str(), c_text.c_str(), static_cast<unsigned long>(c_text.size())};

    CXIndex index = clang_createIndex(0, 0);
    CXTranslationUnit tu = nullptr;
    double start = now_ms();
    CXErrorCode code = clang_parseTranslationUnit2(index, file_name.c_str(), args.data(), static_cast<int>(args.size()),
                                                   &unsaved, 1, CXTranslationUnit_None, &tu);
    unit.parse_ms = now_ms() - start;
    if (code != CXError_Success || !tu) {
        unit.error = "libclang could not parse the program (error " + std::to_string(static_cast<int>(code)) + ")";
        clang_disposeIndex(index);
        return unit;
    }

    // Clang's own warnings, where they point into the program (not into the prelude).
    unsigned count = clang_getNumDiagnostics(tu);
    for (unsigned i = 0; i < count; ++i) {
        CXDiagnostic d = clang_getDiagnostic(tu, i);
        CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(d);
        CXSourceLocation loc = clang_getDiagnosticLocation(d);
        if (severity >= CXDiagnostic_Warning) {
            ClangDiagnostic out;
            std::string file = presumed_file(loc, &out.line, &out.col);
            bool ours = from_parc ? file == display_file : clang_Location_isFromMainFile(loc) != 0;
            if (ours) {
                out.severity = severity == CXDiagnostic_Warning ? "warning" : "error";
                out.message = take(clang_getDiagnosticSpelling(d));
                if (from_parc) out.message = parc_names(out.message);
                out.option = take(clang_getDiagnosticOption(d, nullptr));
                unit.diagnostics.push_back(std::move(out));
            }
        }
        clang_disposeDiagnostic(d);
    }

    Converter converter(c_text, from_parc);
    for (CXCursor top : children_of(clang_getTranslationUnitCursor(tu))) {
        if (clang_getCursorKind(top) != CXCursor_FunctionDecl || !clang_isCursorDefinition(top)) continue;
        if (!clang_Location_isFromMainFile(clang_getCursorLocation(top))) continue;
        std::string name = take(clang_getCursorSpelling(top));
        if (from_parc) {
            if (name.rfind("f_", 0) != 0) continue;  // the prelude and the C main
            name = name.substr(2);
        }
        CFunction fn;
        fn.name = name;
        fn.line = Converter::line_of(top);
        for (CXCursor kid : children_of(top)) {
            CXCursorKind k = clang_getCursorKind(kid);
            if (k == CXCursor_ParmDecl) fn.params.push_back(converter.convert(kid));
            else if (k == CXCursor_CompoundStmt) fn.body = converter.convert(kid);
        }
        unit.functions.push_back(std::move(fn));
    }
    unit.ok = true;
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return unit;
}

#else

CUnit parse_c(const std::string&, const std::string&, bool from_parc) {
    CUnit unit;
    unit.parc = from_parc;
    unit.error = "this parc was built without libclang (see PARC_LLVM_ROOT in CMakeLists.txt)";
    return unit;
}

#endif

// ---------------------------------------------------------------- clang -O2 and its remarks

namespace {

#ifdef PARC_HAVE_LLVM_REMARKS

std::string text_of(LLVMRemarkStringRef s) {
    return s ? std::string(LLVMRemarkStringGetData(s), LLVMRemarkStringGetLen(s)) : std::string();
}

// Passes whose remarks are about the program, not about the machine code's layout.
bool interesting(const std::string& pass, const std::string& type) {
    static const std::set<std::string> skipped = {"asm-printer", "prologepilog", "regalloc", "size-info", "sdagisel",
                                                  "annotation-remarks", "stack-frame-layout"};
    if (skipped.count(pass)) return false;
    return type != "analysis" || pass == "loop-vectorize";
}

std::string read_remarks(const std::string& yaml, const std::string& display_file, std::vector<Remark>& out) {
    LLVMRemarkParserRef parser = LLVMRemarkParserCreateYAML(yaml.data(), yaml.size());
    std::set<std::tuple<int, std::string, std::string>> seen;
    while (LLVMRemarkEntryRef entry = LLVMRemarkParserGetNext(parser)) {
        Remark r;
        switch (LLVMRemarkEntryGetType(entry)) {
            case LLVMRemarkTypePassed: r.type = "passed"; break;
            case LLVMRemarkTypeMissed: r.type = "missed"; break;
            default: r.type = "analysis"; break;
        }
        r.pass = text_of(LLVMRemarkEntryGetPassName(entry));
        r.name = text_of(LLVMRemarkEntryGetRemarkName(entry));
        r.function = parc_names(text_of(LLVMRemarkEntryGetFunctionName(entry)));
        std::string file;
        if (LLVMRemarkDebugLocRef loc = LLVMRemarkEntryGetDebugLoc(entry)) {
            file = text_of(LLVMRemarkDebugLocGetSourceFilePath(loc));
            r.line = static_cast<int>(LLVMRemarkDebugLocGetSourceLine(loc));
        }
        for (LLVMRemarkArgRef arg = LLVMRemarkEntryGetFirstArg(entry); arg; arg = LLVMRemarkEntryGetNextArg(arg, entry))
            r.message += text_of(LLVMRemarkArgGetValue(arg));
        // The YAML reader leaves quotes doubled ('' for '); show them once.
        for (size_t at = r.message.find("''"); at != std::string::npos; at = r.message.find("''", at + 1))
            r.message.erase(at, 1);
        r.message = parc_names(r.message);
        LLVMRemarkEntryDispose(entry);
        bool ours = file == display_file || std::filesystem::path(file).filename() == display_file;
        if (ours && interesting(r.pass, r.type) && seen.insert({r.line, r.pass, r.message}).second)
            out.push_back(std::move(r));
    }
    std::string error;
    if (LLVMRemarkParserHasError(parser)) error = LLVMRemarkParserGetErrorMessage(parser);
    LLVMRemarkParserDispose(parser);
    std::stable_sort(out.begin(), out.end(), [](const Remark& a, const Remark& b) { return a.line < b.line; });
    return error;
}

#endif

}  // namespace

NativeBuild build_native(const std::string& c_text, const std::string& display_file, const std::string& folder) {
    namespace fs = std::filesystem;
    NativeBuild build;
    build.clang = clang_path();
    std::error_code ignored;
    fs::create_directories(folder, ignored);
    fs::path c_file = fs::path(folder) / "program.c";
    fs::path yaml = fs::path(folder) / "program.opt.yaml";
    fs::path log = fs::path(folder) / "clang.txt";
#ifdef _WIN32
    fs::path exe = fs::path(folder) / "program.exe";
#else
    fs::path exe = fs::path(folder) / "program";
#endif
    {
        std::ofstream out(c_file, std::ios::binary);
        out << c_text;
    }
    fs::remove(exe, ignored);
    // The same flags as the evaluation: C11, overflow wraps as on the VM. -O2 for the optimizer's full work.
    ProcessRun run = run_process({build.clang, "-std=c11", "-O2", "-fwrapv", "-w", c_file.string(), "-o", exe.string(),
                                  "-fsave-optimization-record", "-foptimization-record-file=" + yaml.string()},
                                 log.string(), log.string());
    build.compile_ms = run.wall_ms;
    if (!run.started) {
        build.error = run.error;
        return build;
    }
    if (run.exit_code != 0 || !fs::exists(exe)) {
        build.error = "clang could not compile the program: " + read_text(log).substr(0, 600);
        return build;
    }
    build.exe = exe.string();
    build.ok = true;
#ifdef PARC_HAVE_LLVM_REMARKS
    std::string error = read_remarks(read_text(yaml), display_file, build.remarks);
    if (!error.empty()) build.error = "the remarks file could not be read completely: " + error;
#else
    (void)display_file;
    build.error = "this parc was built without LLVM's remarks library, so Clang's optimization remarks are not shown";
#endif
    return build;
}

}  // namespace parc::metrics
