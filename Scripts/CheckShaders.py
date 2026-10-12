#!/usr/bin/env python3
# Copyright Fallen Signal Studios. All Rights Reserved.
"""
Compiles Grungify's shader code without Unreal Engine.

UE compiles Grungify.ush only through the Custom expressions the content builder writes into MF_GrungifyWeathering,
MF_GrungifyLandscape, MF_GrungifySubstrate, MF_GrungifyDisplacement and M_GrungifyMaskDebug. This script reads
those node bodies straight out of GrungifyContentBuilder.cpp, wraps each one the way UE wraps a Custom expression
(a function taking the material parameters and one argument per input, with extra outputs as inout arguments),
and compiles the result with glslang's HLSL front end for pixel and vertex stages.

It catches syntax errors, undeclared names, wrong argument counts and types, missing returns, and constructs UE's
newer HLSL 2021 mode rejects (vector conditions in ?:, &&, ||). It cannot check UE-specific semantics such as
which inputs the graph actually connects; the in-engine content report still does that.

Requires glslangValidator (Ubuntu: apt install glslang-tools). Exit code 0 when every variant compiles.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
USH = os.path.join(ROOT, "Shaders", "Private", "Grungify.ush")
BUILDER = os.path.join(ROOT, "Source", "GrungifyEditor", "Private", "GrungifyContentBuilder.cpp")

# Custom-node inputs take the type of whatever feeds them. Everything not listed is a scalar.
INPUT_TYPES = {
    "BaseColor": "float3", "NormalTS": "float3", "WorldPos": "float3", "WorldNormal": "float3",
    "MaskUV": "float2",
    # Vector parameters feed RGBA; bodies read .rgb.
    "DustColor": "float4", "RunoffColor": "float4", "ChipColor": "float4", "StoneChipColor": "float4",
    "MossColor": "float4", "SootColor": "float4", "SaltColor": "float4",
}
OUTPUT_TYPES = {"CMOT_Float1": "float", "CMOT_Float2": "float2", "CMOT_Float3": "float3", "CMOT_Float4": "float4"}

# A stand-in for the part of UE's material template the node bodies may touch.
PRELUDE = """
#define MaterialFloat float
#define MaterialFloat2 float2
#define MaterialFloat3 float3
#define MaterialFloat4 float4
struct FMaterialPixelParameters
{
    float3x3 TangentToWorld;
    float3 AbsoluteWorldPosition;
};
"""


def fail(message):
    print("ERROR: " + message)
    sys.exit(1)


def cpp_string(literal):
    """Decodes the inside of a C++ string literal (the escapes the builder uses)."""
    return (literal.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"').replace("\\\\", "\\"))


def text_literals(expression):
    return "".join(cpp_string(m) for m in re.findall(r'TEXT\("((?:[^"\\]|\\.)*)"\)', expression))


def named_string(source, name):
    """const TCHAR* Name = TEXT("...") TEXT("...");"""
    match = re.search(r"const TCHAR\*\s+" + name + r"\s*=(.*?);\n", source, re.S)
    if not match:
        fail("could not find %s in the content builder" % name)
    return text_literals(match.group(1))


def split_arguments(text, open_index):
    """Splits a call's arguments starting at the '(' at open_index, honouring nesting and string literals."""
    depth, args, current, i = 0, [], [], open_index
    in_string = False
    while i < len(text):
        c = text[i]
        if in_string:
            current.append(c)
            if c == "\\":
                current.append(text[i + 1])
                i += 1
            elif c == '"':
                in_string = False
        elif c == '"':
            in_string = True
            current.append(c)
        elif c in "([{":
            depth += 1
            if depth > 1:
                current.append(c)
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append("".join(current).strip())
                return args
            current.append(c)
        elif c == "," and depth == 1:
            args.append("".join(current).strip())
            current = []
        else:
            current.append(c)
        i += 1
    fail("unbalanced call in the content builder")


def evaluate_body(expression, strings):
    """FString(EvaluateCode) + TEXT("...") and friends."""
    result = ""
    for part in re.findall(r'FString\((\w+)\)|(\b\w+Code\b)|TEXT\("((?:[^"\\]|\\.)*)"\)', expression):
        name = part[0] or part[1]
        if name:
            if name not in strings:
                fail("unknown code constant %s" % name)
            result += strings[name]
        else:
            result += cpp_string(part[2])
    return result


def collect_nodes(source):
    weathering = re.findall(r'TEXT\("(\w+)"\)', re.search(r"WeatheringInputNames\s*=\s*\{(.*?)\};", source, re.S).group(1))
    if len(weathering) < 40:
        fail("WeatheringInputNames looks wrong (%d names)" % len(weathering))
    strings = {"CommonCode": named_string(source, "CommonCode"), "EvaluateCode": named_string(source, "EvaluateCode")}
    if "Body" not in re.search(r"Custom->Code\s*=\s*([^;]*);", source).group(1):
        fail("AddWeatheringCustom no longer builds Code as CommonCode + Body; update this script")

    name_lists = {
        None: weathering,
        "SubstrateInputNames()": [n for n in weathering if n not in ("BaseColor", "Roughness", "Metallic")] + ["StoneChipColor"],
        "Names": [n for n in weathering if n not in ("BaseColor", "Roughness", "Metallic", "NormalTS", "Relief")]
                 + ["Displacement", "DisplacementDepth"],
    }
    # Guard the hand-written lists above against drift in the builder.
    if "Names.Add(TEXT(\"Displacement\"));" not in source or "Names.Add(TEXT(\"DisplacementDepth\"));" not in source:
        fail("the displacement input list changed; update this script")
    substrate = re.search(r"TArray<const TCHAR\*> SubstrateInputNames\(\)(.*?)\n\t\}", source, re.S).group(1)
    if "StoneChipColor" not in substrate:
        fail("SubstrateInputNames no longer adds StoneChipColor; update this script")

    nodes = []
    for match in re.finditer(r"AddWeatheringCustom\(", source):
        if source[max(0, match.start() - 30):match.start()].rstrip().endswith("*"):
            continue  # the definition, not a call
        args = split_arguments(source, match.end() - 1)
        if len(args) < 8:
            continue
        description = text_literals(args[3])
        output = OUTPUT_TYPES.get(args[4])
        if not output:
            fail("%s: unknown output type %s" % (description, args[4]))
        names_arg = args[8] if len(args) > 8 else None
        if names_arg not in name_lists:
            fail("%s: unknown input list %s; teach this script about it" % (description, names_arg))
        extra = []
        if description == "Grungify Substrate Grime":
            extra = [("Albedo", "float3"), ("F0", "float3"), ("GrimeRoughness", "float")]
        nodes.append((description, output, name_lists[names_arg], strings["CommonCode"] + evaluate_body(args[5], strings), extra))

    debug = re.search(r'Custom->Description = TEXT\("Grungify Mask Debug"\);(.*?)Ctx\.Connect', source, re.S).group(1)
    debug_code = text_literals(re.search(r"Custom->Code\s*=(.*?);\n", debug, re.S).group(1))
    debug_inputs = re.findall(r'TEXT\("(\w+)"\)', re.search(r"SetCustomInputs\(Custom,\s*\{(.*?)\}\);", debug, re.S).group(1))
    nodes.append(("Grungify Mask Debug", "float3", debug_inputs, debug_code, []))

    descriptions = [n[0] for n in nodes]
    for expected in ("Grungify Base Color", "Grungify Roughness Metallic", "Grungify Relief Normal",
                     "Grungify Substrate Grime", "Grungify Displacement", "Grungify Mask Debug"):
        if expected not in descriptions:
            fail("did not find the %s node; update this script" % expected)
    return nodes


def wrap(index, description, output, inputs, body, extra):
    params = ["FMaterialPixelParameters Parameters"]
    params += ["%s %s" % (INPUT_TYPES.get(name, "float"), name) for name in inputs]
    params += ["inout %s %s" % (kind, name) for name, kind in extra]
    call_args = ["P"] + ["(%s)0" % INPUT_TYPES.get(name, "float") for name in inputs] + [name for name, _ in extra]
    locals_ = "".join("    %s %s = (%s)0;\n" % (kind, name, kind) for name, kind in extra)
    sink = "float(Out%d.x)" % index if output != "float" else "Out%d" % index
    function = "// %s\n%s CustomExpression%d(%s)\n{\n%s\n}\n" % (description, output, index, ", ".join(params), body)
    caller = "    %s Out%d = CustomExpression%d(%s);\n    Sum += %s;\n" % (output, index, index, ", ".join(call_args), sink)
    return function, locals_ + caller


def hlsl2021_lint(path, text):
    """Vector conditions in ?:, && and || are errors in HLSL 2021 (UE uses it on newer RHIs). Flag likely cases."""
    problems = []
    for number, line in enumerate(text.splitlines(), 1):
        code = line.split("//")[0]
        if re.search(r"\b(float|half|int|uint|bool)[234]\s*\([^;]*\)\s*\?", code) or re.search(r"\.(xy|xyz|rgb|rgba|xyzw)\s*[<>]=?[^;?]*\?", code):
            problems.append("%s:%d: vector ?: needs select() under HLSL 2021: %s" % (path, number, code.strip()))
        if re.search(r"\b(float|bool)[234]\s*\([^)]*\)\s*(&&|\|\|)", code):
            problems.append("%s:%d: vector &&/|| needs and()/or() under HLSL 2021: %s" % (path, number, code.strip()))
    return problems


def main():
    tool = shutil.which("glslangValidator")
    if not tool:
        fail("glslangValidator not found (apt install glslang-tools)")
    with open(BUILDER, encoding="utf-8") as handle:
        source = handle.read()
    with open(USH, encoding="utf-8") as handle:
        ush = handle.read()
    nodes = collect_nodes(source)

    functions, calls = [], []
    for index, node in enumerate(nodes):
        function, call = wrap(index, *node)
        functions.append(function)
        calls.append(call)

    shader = PRELUDE + '#include "Grungify.ush"\n\n' + "\n".join(functions)
    pixel = shader + "\nfloat4 MainPS(float4 Position : SV_Position) : SV_Target0\n{\n    FMaterialPixelParameters P = (FMaterialPixelParameters)0;\n    float Sum = 0;\n" + "".join(calls) + "    return float4(Sum, 0, 0, 1);\n}\n"
    vertex = shader + "\nfloat4 MainVS(float4 Position : POSITION) : SV_Position\n{\n    FMaterialPixelParameters P = (FMaterialPixelParameters)0;\n    float Sum = 0;\n" + "".join(calls) + "    return Position + Sum;\n}\n"

    problems = hlsl2021_lint("Grungify.ush", ush)
    for description, _, _, body, _ in nodes:
        problems += hlsl2021_lint(description, body)

    failed = False
    with tempfile.TemporaryDirectory() as work:
        shutil.copy(USH, os.path.join(work, "Grungify.ush"))
        for stage, entry, text in (("frag", "MainPS", pixel), ("vert", "MainVS", vertex)):
            path = os.path.join(work, "GrungifyCheck.%s.hlsl" % stage)
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(text)
            result = subprocess.run([tool, "-D", "-V", "-S", stage, "-e", entry, "-I" + work, "--hlsl-iomap", "-o", os.path.join(work, stage + ".spv"), path],
                                    capture_output=True, text=True)
            output = (result.stdout + result.stderr).strip()
            if result.returncode != 0:
                failed = True
                print("FAIL %s stage:\n%s" % (stage, output))
                with open(os.path.join(ROOT, "Saved_ShaderCheck_%s.hlsl" % stage) if os.environ.get("GRUNGIFY_KEEP_SHADER") else os.devnull, "w") as keep:
                    keep.write(text)
            else:
                print("ok   %s stage: Grungify.ush + %d custom nodes" % (stage, len(nodes)))

    for problem in problems:
        print("FAIL " + problem)
    for description, output, inputs, _, extra in nodes:
        print("     %-28s %-6s %2d inputs%s" % (description, output, len(inputs), " + %d outputs" % len(extra) if extra else ""))
    if failed or problems:
        print("SHADERS FAIL")
        return 1
    print("SHADERS OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
