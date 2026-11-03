"""Random differential testing of the ParC compiler against itself.

Generates random (but always valid and always terminating) ParC programs and
runs each one four ways: with and without the optimizer, with many and with
very few registers, on one thread and on four. All four runs must print exactly
the same text. A difference means a bug in the optimizer, the register
allocator or the parallel runtime, and the program is saved for inspection.

    python evaluation/fuzz.py              200 programs, seed 1
    python evaluation/fuzz.py 1000 7       1000 programs, seed 7
"""

import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PARC = ROOT / "build" / "msvc" / "Release" / "parc.exe"

CONFIGURATIONS = [
    ["-O0", "--threads", "1"],                    # the reference: no optimizer, serial
    [],                                           # optimized, 12 registers, 4 threads
    ["--regs", "4", "--threads", "3"],            # optimized, almost everything spilled
    ["-O0", "--regs", "5", "--threads", "4"],
]


class Generator:
    """Builds one random program. Every expression is safe by construction:
    divisors are never zero and array indexes are always inside the array."""

    def __init__(self, rng):
        self.rng = rng
        self.lines = []
        self.indent = 1
        self.names = 0
        self.functions = []  # (name, number of parameters)
        self.quiet_functions = False  # True: only main prints (used when comparing with C)
        self.in_function = False

    def emit(self, text):
        self.lines.append("    " * self.indent + text)

    def fresh(self, prefix):
        self.names += 1
        return f"{prefix}{self.names}"

    def expr(self, ints, depth=0):
        """An int expression over the variables in `ints`."""
        rng = self.rng
        if depth >= 3 or rng.random() < 0.3:
            if ints and rng.random() < 0.7:
                return rng.choice(ints)
            return str(rng.choice([0, 1, 2, 3, 5, 7, 10, 100, 1000, 65536, 2147483647]))
        kind = rng.random()
        a = self.expr(ints, depth + 1)
        b = self.expr(ints, depth + 1)
        if kind < 0.55:
            return f"({a} {rng.choice(['+', '-', '*'])} {b})"
        if kind < 0.75:
            # b % 5 is between -4 and 4, so the divisor is between 3 and 11: never zero.
            return f"({a} {rng.choice(['/', '%'])} ({b} % 5 + 7))"
        if kind < 0.85:
            return f"(-{a})"
        if kind < 0.95 and self.functions:
            name, count = rng.choice(self.functions)
            args = ", ".join(self.expr(ints, depth + 1) for _ in range(count))
            return f"{name}({args})"
        return f"({a} * 2 + {b})"

    def condition(self, ints):
        rng = self.rng
        test = f"{self.expr(ints, 2)} {rng.choice(['<', '<=', '>', '>=', '==', '!='])} {self.expr(ints, 2)}"
        if rng.random() < 0.3:
            other = f"{self.expr(ints, 2)} {rng.choice(['<', '>', '!='])} {self.expr(ints, 2)}"
            test = f"{test} {rng.choice(['&&', '||'])} {other}"
        if rng.random() < 0.15:
            test = f"!({test})"
        return test

    def index(self, ints, size):
        """An index that is always in 0 .. size-1."""
        return f"(({self.expr(ints, 2)} % {size} + {size}) % {size})"

    def block(self, ints, writable, arrays, depth, budget):
        """A few statements. `writable` are the variables assignment may change
        (loop variables are readable but not writable)."""
        rng = self.rng
        ints = list(ints)
        writable = list(writable)
        for _ in range(rng.randint(1, budget)):
            kind = rng.random()
            if kind < 0.25:
                name = self.fresh("v")
                self.emit(f"let {name}: int = {self.expr(ints)};")
                ints.append(name)
                writable.append(name)
            elif kind < 0.45 and writable:
                self.emit(f"{rng.choice(writable)} = {self.expr(ints)};")
            elif kind < 0.55 and arrays:
                array, size = rng.choice(arrays)
                self.emit(f"{array}[{self.index(ints, size)}] = {self.expr(ints)};")
            elif kind < 0.62 and arrays and writable:
                array, size = rng.choice(arrays)
                self.emit(f"{rng.choice(writable)} = {array}[{self.index(ints, size)}];")
            elif kind < 0.72:
                if not (self.quiet_functions and self.in_function):
                    self.emit(f"print({', '.join(self.expr(ints, 2) for _ in range(rng.randint(1, 3)))});")
            elif kind < 0.82 and depth < 3:
                self.emit(f"if ({self.condition(ints)}) {{")
                self.indent += 1
                self.block(ints, writable, arrays, depth + 1, 3)
                self.indent -= 1
                if rng.random() < 0.5:
                    self.emit("} else {")
                    self.indent += 1
                    self.block(ints, writable, arrays, depth + 1, 3)
                    self.indent -= 1
                self.emit("}")
            elif kind < 0.90 and depth < 2:
                var = self.fresh("i")
                self.emit(f"for {var} in {rng.randint(0, 3)}..{rng.randint(0, 9)} {{")
                self.indent += 1
                self.block(ints + [var], writable, arrays, depth + 1, 3)
                if rng.random() < 0.2:
                    self.emit(f"if ({var} == {rng.randint(0, 6)}) {{ {rng.choice(['break', 'continue'])}; }}")
                self.indent -= 1
                self.emit("}")
            elif kind < 0.95 and depth < 2:
                counter = self.fresh("w")
                self.emit(f"let {counter}: int = 0;")
                self.emit(f"while ({counter} < {rng.randint(1, 8)}) {{")
                self.indent += 1
                self.block(ints + [counter], writable, arrays, depth + 1, 3)
                self.emit(f"{counter} = {counter} + 1;")
                self.indent -= 1
                self.emit("}")
            elif depth < 2:
                self.parallel_loop(ints, writable, arrays)
        return ints, writable

    def parallel_loop(self, ints, writable, arrays):
        """A parallel loop that is safe: a reduction, or one element per iteration."""
        rng = self.rng
        var = self.fresh("p")
        # No calls inside: a called function may print, and the order of lines
        # printed from a parallel loop depends on the number of threads.
        callable_functions, self.functions = self.functions, []
        try:
            self.parallel_body(ints, arrays, var)
        finally:
            self.functions = callable_functions

    def parallel_body(self, ints, arrays, var):
        rng = self.rng
        if arrays and rng.random() < 0.5:
            array, size = rng.choice(arrays)
            self.emit(f"parallel for {var} in 0..{size} {{")
            self.emit(f"    {array}[{var}] = {self.expr(ints + [var], 1)};")
            self.emit("}")
        else:
            total = self.fresh("r")
            op = rng.choice(["+", "*"])
            self.emit(f"let {total}: int = {rng.randint(0, 5)};")
            self.emit(f"parallel for {var} in {rng.randint(0, 2)}..{rng.randint(3, 20)} reduce({op}: {total}) {{")
            self.emit(f"    {total} = {total} {op} {self.expr(ints + [var], 1)};")
            self.emit("}")
            if not (self.quiet_functions and self.in_function):
                self.emit(f"print({total});")

    def function(self, name, params):
        self.lines.append(f"fn {name}({', '.join(p + ': int' for p in params)}) -> int {{")
        self.in_function = True
        ints, _ = self.block(params, params, [], 1, 5)
        self.in_function = False
        self.emit(f"return {self.expr(ints)};")
        self.lines.append("}")
        self.lines.append("")
        self.functions.append((name, len(params)))

    def program(self):
        for n in range(self.rng.randint(0, 3)):
            self.function(f"f{n}", [f"a{n}", f"b{n}"][: self.rng.randint(1, 2)])
        self.lines.append("fn main() {")
        arrays = []
        for _ in range(self.rng.randint(0, 2)):
            name = self.fresh("arr")
            size = self.rng.randint(1, 9)
            self.emit(f"let {name}: int[] = new int[{size}];")
            arrays.append((name, size))
        ints, _ = self.block([], [], arrays, 0, 8)
        for name, size in arrays:
            self.emit(f"print({', '.join(f'{name}[{k}]' for k in range(min(size, 4)))});")
        if ints:
            self.emit(f"print({', '.join(ints[:6])});")
        self.lines.append("}")
        return "\n".join(self.lines) + "\n"


def run(path, options):
    done = subprocess.run([str(PARC), "run", str(path), "--max-steps", "5000000", *options],
                          capture_output=True, text=True, timeout=60)
    errors = [line for line in done.stderr.splitlines() if "runtime error" in line or "error:" in line]
    return done.returncode, done.stdout, errors


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    if not PARC.is_file():
        sys.exit(f"build the compiler first: {PARC} is missing")
    rng = random.Random(seed)
    failures = 0
    statements = 0
    with tempfile.TemporaryDirectory(prefix="parc_fuzz_") as folder:
        path = Path(folder) / "program.parc"
        for number in range(count):
            source = Generator(rng).program()
            statements += source.count(";")
            path.write_text(source, encoding="utf-8")
            reference = run(path, CONFIGURATIONS[0])
            if reference[0] not in (0, 1) or any("error:" in e and "runtime error" not in e for e in reference[2]):
                failures += 1
                keep = ROOT / "evaluation" / f"fuzz_failure_{seed}_{number}.parc"
                keep.write_text(source, encoding="utf-8")
                print(f"program {number}: the compiler rejected a generated program, saved as {keep.name}")
                print("   ", reference[2][:3])
                continue
            for options in CONFIGURATIONS[1:]:
                result = run(path, options)
                if result[:2] == reference[:2]:
                    continue
                failures += 1
                keep = ROOT / "evaluation" / f"fuzz_failure_{seed}_{number}.parc"
                keep.write_text(source, encoding="utf-8")
                print(f"program {number}: output differs with options {options or ['(defaults)']}, saved as {keep.name}")
                break
    print(f"{count} programs ({statements} statements), {len(CONFIGURATIONS)} configurations each: "
          f"{failures} failed" if failures else
          f"{count} programs ({statements} statements), {len(CONFIGURATIONS)} configurations each: all agree")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
