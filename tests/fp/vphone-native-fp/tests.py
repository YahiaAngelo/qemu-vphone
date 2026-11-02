#!/usr/bin/env python3
# Generates tests.inc: one asm stub per instruction form, and the table main.c walks.
# Stub convention: v0 = n, v1 = m, v2 = a, v3 = d (pre-filled with junk, stored back
# whole, so upper-lane zeroing is checked too); x9 = integer input, x10 = integer output.
import sys

tests = []   # (name, esz, kind, asm)

def t(name, esz, asm, kind="fp"):
    tests.append((name, esz, kind, asm))

S = {1: "h", 2: "s", 3: "d"}
V = {(2, 0): "2s", (2, 1): "4s", (3, 1): "2d", (1, 0): "4h", (1, 1): "8h"}

# Scalar three-operand
for op in ["fadd", "fsub", "fmul", "fdiv", "fmax", "fmin", "fmaxnm", "fminnm",
           "fmulx", "fnmul", "fabd", "fcmeq", "fcmge", "fcmgt", "facge", "facgt",
           "frecps", "frsqrts"]:
    for e in (1, 2, 3):
        r = S[e]
        t(f"{op}_{r}", e, f"{op} {r}3, {r}0, {r}1")
# Scalar fused
for op in ["fmadd", "fmsub", "fnmadd", "fnmsub"]:
    for e in (1, 2, 3):
        r = S[e]
        t(f"{op}_{r}", e, f"{op} {r}3, {r}0, {r}1, {r}2")
# Scalar one-operand
for op in ["fsqrt", "frintn", "frintp", "frintm", "frintz", "frinta", "frinti",
           "frintx", "fabs", "fneg", "frecpe", "frsqrte", "frecpx"]:
    for e in (1, 2, 3):
        r = S[e]
        t(f"{op}_{r}", e, f"{op} {r}3, {r}0")
for op in ["frint32z", "frint32x", "frint64z", "frint64x"]:
    for e in (2, 3):
        r = S[e]
        t(f"{op}_{r}", e, f"{op} {r}3, {r}0")
# Scalar compare with zero
for op, z in [("fcmeq", "#0.0"), ("fcmge", "#0.0"), ("fcmgt", "#0.0"),
              ("fcmle", "#0.0"), ("fcmlt", "#0.0")]:
    for e in (1, 2, 3):
        r = S[e]
        t(f"{op}0_{r}", e, f"{op} {r}3, {r}0, {z}")
# Precision conversions
for d, s_ in [("d", "s"), ("s", "d"), ("h", "s"), ("s", "h"), ("h", "d"), ("d", "h")]:
    e = {"h": 1, "s": 2, "d": 3}[s_]
    t(f"fcvt_{d}{s_}", e, f"fcvt {d}3, {s_}0")
t("fcvtxn_sd", 3, "fcvtxn s3, d0")
# Compares into NZCV
for e in (1, 2, 3):
    r = S[e]
    t(f"fcmp_{r}", e, f"fcmp {r}0, {r}1\n mrs x10, nzcv\n fmov d3, x10")
    t(f"fcmpe_{r}", e, f"fcmpe {r}0, {r}1\n mrs x10, nzcv\n fmov d3, x10")
    t(f"fcmp0_{r}", e, f"fcmp {r}0, #0.0\n mrs x10, nzcv\n fmov d3, x10")
    t(f"fcmpe0_{r}", e, f"fcmpe {r}0, #0.0\n mrs x10, nzcv\n fmov d3, x10")
    t(f"fccmp_{r}", e, f"fmov x9, d2\n tst x9, #1\n fccmp {r}0, {r}1, #5, ne\n mrs x10, nzcv\n fmov d3, x10")
    t(f"fccmpe_{r}", e, f"fmov x9, d2\n tst x9, #2\n fccmpe {r}0, {r}1, #10, eq\n mrs x10, nzcv\n fmov d3, x10")
# To and from general registers
for e in (1, 2, 3):
    r = S[e]
    for cv in ["fcvtns", "fcvtnu", "fcvtps", "fcvtpu", "fcvtms", "fcvtmu",
               "fcvtzs", "fcvtzu", "fcvtas", "fcvtau"]:
        for g in ("w", "x"):
            t(f"{cv}_{g}{r}", e, f"{cv} {g}10, {r}0\n fmov d3, x10")
    for g in ("w", "x"):
        t(f"fcvtzs_{g}{r}_fix", e, f"fcvtzs {g}10, {r}0, #7\n fmov d3, x10")
        t(f"fcvtzu_{g}{r}_fix", e, f"fcvtzu {g}10, {r}0, #3\n fmov d3, x10")
        t(f"scvtf_{r}{g}", e, f"fmov x9, d0\n scvtf {r}3, {g}9", kind="int")
        t(f"ucvtf_{r}{g}", e, f"fmov x9, d0\n ucvtf {r}3, {g}9", kind="int")
        t(f"scvtf_{r}{g}_fix", e, f"fmov x9, d0\n scvtf {r}3, {g}9, #5", kind="int")
    # scalar forms on FP registers
    t(f"scvtf_{r}{r}", e, f"scvtf {r}3, {r}0", kind="int")
    t(f"ucvtf_{r}{r}", e, f"ucvtf {r}3, {r}0", kind="int")
    t(f"fcvtzs_{r}{r}", e, f"fcvtzs {r}3, {r}0")
    t(f"fcvtns_{r}{r}", e, f"fcvtns {r}3, {r}0")
    t(f"fcvtmu_{r}{r}", e, f"fcvtmu {r}3, {r}0")
# Scalar pairwise and by element
for op in ["faddp", "fmaxp", "fminp", "fmaxnmp", "fminnmp"]:
    t(f"{op}_s", 2, f"{op} s3, v0.2s")
    t(f"{op}_d", 3, f"{op} d3, v0.2d")
    t(f"{op}_h", 1, f"{op} h3, v0.2h")
for op in ["fmul", "fmulx", "fmla", "fmls"]:
    pre = "mov v3.16b, v2.16b\n " if op in ("fmla", "fmls") else ""
    t(f"{op}_s_idx", 2, f"{pre}{op} s3, s0, v1.s[3]")
    t(f"{op}_d_idx", 3, f"{pre}{op} d3, d0, v1.d[1]")
    t(f"{op}_h_idx", 1, f"{pre}{op} h3, h0, v1.h[5]")
# Vector three-operand
for op in ["fadd", "fsub", "fmul", "fdiv", "fmax", "fmin", "fmaxnm", "fminnm",
           "fmulx", "fabd", "fcmeq", "fcmge", "fcmgt", "facge", "facgt",
           "frecps", "frsqrts", "faddp", "fmaxp", "fminp", "fmaxnmp", "fminnmp",
           "fmla", "fmls"]:
    for (e, q), a in V.items():
        pre = "mov v3.16b, v2.16b\n " if op in ("fmla", "fmls") else ""
        t(f"{op}_{a}", e, f"{pre}{op} v3.{a}, v0.{a}, v1.{a}")
# Vector by element
for op in ["fmul", "fmulx", "fmla", "fmls"]:
    pre = "mov v3.16b, v2.16b\n " if op in ("fmla", "fmls") else ""
    t(f"{op}_2s_idx", 2, f"{pre}{op} v3.2s, v0.2s, v1.s[1]")
    t(f"{op}_4s_idx", 2, f"{pre}{op} v3.4s, v0.4s, v1.s[2]")
    t(f"{op}_2d_idx", 3, f"{pre}{op} v3.2d, v0.2d, v1.d[1]")
    t(f"{op}_8h_idx", 1, f"{pre}{op} v3.8h, v0.8h, v1.h[6]")
# Integer multiply by element (has a native path too)
for op in ["mul", "mla", "mls"]:
    pre = "mov v3.16b, v2.16b\n " if op != "mul" else ""
    t(f"{op}_4h_idx", 1, f"{pre}{op} v3.4h, v0.4h, v1.h[7]", kind="int")
    t(f"{op}_8h_idx", 1, f"{pre}{op} v3.8h, v0.8h, v1.h[2]", kind="int")
    t(f"{op}_2s_idx", 2, f"{pre}{op} v3.2s, v0.2s, v1.s[3]", kind="int")
    t(f"{op}_4s_idx", 2, f"{pre}{op} v3.4s, v0.4s, v1.s[0]", kind="int")
# Vector one-operand
for op in ["fsqrt", "frintn", "frintp", "frintm", "frintz", "frinta", "frinti",
           "frintx", "fabs", "fneg", "frecpe", "frsqrte",
           "fcvtns", "fcvtnu", "fcvtps", "fcvtpu", "fcvtms", "fcvtmu",
           "fcvtzs", "fcvtzu", "fcvtas", "fcvtau"]:
    for (e, q), a in V.items():
        t(f"{op}_{a}", e, f"{op} v3.{a}, v0.{a}")
for op in ["frint32z", "frint64x"]:
    for (e, q), a in V.items():
        if e > 1:
            t(f"{op}_{a}", e, f"{op} v3.{a}, v0.{a}")
for op in ["scvtf", "ucvtf"]:
    for (e, q), a in V.items():
        t(f"{op}_{a}", e, f"{op} v3.{a}, v0.{a}", kind="int")
        t(f"{op}_{a}_fix", e, f"{op} v3.{a}, v0.{a}, #3", kind="int")
for (e, q), a in V.items():
    t(f"fcvtzs_{a}_fix", e, f"fcvtzs v3.{a}, v0.{a}, #4")
    t(f"fcvtzu_{a}_fix", e, f"fcvtzu v3.{a}, v0.{a}, #2")
    for op in ["fcmeq", "fcmge", "fcmgt", "fcmle", "fcmlt"]:
        t(f"{op}0_{a}", e, f"{op} v3.{a}, v0.{a}, #0.0")
# Narrowing and lengthening
t("fcvtn_2s", 3, "fcvtn v3.2s, v0.2d")
t("fcvtn2_4s", 3, "mov v3.16b, v2.16b\n fcvtn2 v3.4s, v0.2d")
t("fcvtn_4h", 2, "fcvtn v3.4h, v0.4s")
t("fcvtn2_8h", 2, "mov v3.16b, v2.16b\n fcvtn2 v3.8h, v0.4s")
t("fcvtxn_2s", 3, "fcvtxn v3.2s, v0.2d")
t("fcvtxn2_4s", 3, "mov v3.16b, v2.16b\n fcvtxn2 v3.4s, v0.2d")
t("fcvtl_2d", 2, "fcvtl v3.2d, v0.2s")
t("fcvtl2_2d", 2, "fcvtl2 v3.2d, v0.4s")
t("fcvtl_4s", 1, "fcvtl v3.4s, v0.4h")
t("fcvtl2_4s", 1, "fcvtl2 v3.4s, v0.8h")
# Reductions
for op in ["fmaxv", "fminv", "fmaxnmv", "fminnmv"]:
    t(f"{op}_4s", 2, f"{op} s3, v0.4s")
    t(f"{op}_8h", 1, f"{op} h3, v0.8h")
    t(f"{op}_4h", 1, f"{op} h3, v0.4h")

# Several guarded native ops in one translation block, results feeding each other.
t("chain_s", 2, "fadd s4, s0, s1\n fmul s5, s4, s2\n fmadd s6, s0, s1, s5\n"
  " fcmp s6, s4\n fcsel s3, s6, s5, gt\n fsqrt s7, s3\n fdiv s3, s7, s1\n"
  " fcvtzs w10, s3\n mov v3.s[1], w10\n fmax s4, s4, s0\n mov v3.s[2], v4.s[0]")
t("chain_d", 3, "fsub d4, d0, d1\n fnmul d5, d4, d2\n fmsub d6, d0, d1, d5\n"
  " fcvt s7, d6\n fcvt d7, s7\n fminnm d3, d7, d4\n frintm d4, d3\n"
  " fcvtzs x10, d4\n mov v3.d[1], x10")
t("chain_v", 2, "fmul v4.4s, v0.4s, v1.4s\n fmla v4.4s, v2.4s, v1.s[1]\n"
  " fadd v5.2s, v4.2s, v0.2s\n fcvtl v6.2d, v5.2s\n fdiv v6.2d, v6.2d, v6.2d\n"
  " fcvtn v3.2s, v6.2d\n fcvtn2 v3.4s, v6.2d\n fcmgt v7.4s, v4.4s, v3.4s\n"
  " bsl v7.16b, v4.16b, v3.16b\n faddp v3.4s, v7.4s, v4.4s")
# FPCR changed between native ops: the rounding mode, and FZ (native on/off).
t("fpcr_rm", 2, "mrs x9, fpcr\n eor x10, x9, #(1 << 22)\n msr fpcr, x10\n"
  " fadd s3, s0, s1\n eor x10, x9, #(3 << 22)\n msr fpcr, x10\n fmul s4, s0, s1\n"
  " msr fpcr, x9\n mov v3.s[1], v4.s[0]")
t("fpcr_fz", 3, "mrs x9, fpcr\n eor x10, x9, #(1 << 24)\n msr fpcr, x10\n"
  " fmul d3, d0, d1\n msr fpcr, x9\n fmul d4, d0, d1\n mov v3.d[1], v4.d[0]")

# The same register for several operands (TCG then forwards one load to the next).
for e in (2, 3):
    r = S[e]
    t(f"fadd_{r}_same", e, f"fadd {r}3, {r}0, {r}0")
    t(f"fmadd_{r}_same", e, f"fmadd {r}3, {r}0, {r}1, {r}1")
    t(f"fmin_{r}_same", e, f"fmin {r}3, {r}1, {r}1")
    t(f"fcmp_{r}_same", e, f"fcmp {r}0, {r}0\n mrs x10, nzcv\n fmov d3, x10")
    t(f"fdiv_{r}_dst", e, f"mov v3.16b, v0.16b\n fdiv {r}3, {r}3, {r}1\n fsqrt {r}3, {r}3")
t("fmul_4s_same", 2, "fmul v3.4s, v0.4s, v0.4s")
t("fmla_2d_same", 3, "mov v3.16b, v2.16b\n fmla v3.2d, v3.2d, v3.2d")
t("fcvtzs_x_then_scvtf", 3, "fcvtzs x10, d0\n scvtf d3, x10\n fcvtzs x9, d3\n scvtf d4, x9\n"
  " fadd d3, d3, d4")

with open(sys.argv[1], "w") as f:
    for i, (name, esz, kind, asm) in enumerate(tests):
        body = "\\n ".join(asm.split("\n "))
        f.write(f'static void t_{i}(void) {{ __asm__ volatile('
                f'"ldr q0, [%0]\\n ldr q1, [%1]\\n ldr q2, [%2]\\n ldr q3, [%3]\\n '
                f'{body}\\n str q3, [%3]" :: "r"(&N), "r"(&M), "r"(&A), "r"(&D) '
                f': "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "x9", "x10", "cc", "memory"); }}\n')
    f.write("static const struct test tests[] = {\n")
    for i, (name, esz, kind, asm) in enumerate(tests):
        f.write(f'    {{ "{name}", t_{i}, {esz}, {1 if kind == "int" else 0} }},\n')
    f.write("};\n")
print(len(tests), "tests")
