#!/usr/bin/env python3
"""Read-only targeted queries against the Combined Fallout Ghidra project.

This script never starts analysis or saves a Program.  It prints the imported
TESWorldSpace layout, selected preload/native-linked decompilations, their
callers, and functions which contain an instruction operand of 0x148 (the
CommonLibF4 TESWorldSpace::teleportDoorCache offset).
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path


GHIDRA_REPO = Path(r"C:\Development\Tools\BethesdaGhidraScripts")
GHIDRA_DIR = GHIDRA_REPO / "tools" / "ghidra"
PROJECT_DIR = r"C:\Ghidraprojects"
PROJECT_NAME = "Combined"

PROGRAMS = {
    "OG": {
        "path": "/Fallout4/Fallout4_OG_1_10_163.exe",
        "rvas": {
            "PreloadWorld": 0xFC0D0,
            "NativeLinkedHelper": 0xEC5440,
            "NativeLinkedOuter": 0xEC5570,
            "EnumTeleportDoorsCloseToPoint": 0x37A860,
        },
    },
    "VR": {
        "path": "/Fallout4/Fallout4VR_1_2_72.exe",
        "rvas": {
            "PreloadWorld": 0xFC0A0,
            "NativeLinkedHelper": 0xF36060,
            "NativeLinkedOuter": 0xF36190,
            "EnumTeleportDoorsCloseToPoint": 0x360F50,
        },
    },
}

NAME_PATTERN = re.compile(
    r"preload|teleport|loadedreference|linkedarea|gridarray",
    re.IGNORECASE,
)


def decompile(interface, function, monitor, timeout=30):
    if function is None:
        return "<no function>"
    result = interface.decompileFunction(function, timeout, monitor)
    if not result or not result.decompileCompleted():
        error = result.getErrorMessage() if result else "no result"
        return f"<decompile failed: {error}>"
    return str(result.getDecompiledFunction().getC())


def print_worldspace_layout(program):
    print("\n-- TESWorldSpace layout around 0x148 --")
    manager = program.getDataTypeManager()
    matches = []
    for data_type in manager.getAllDataTypes():
        if data_type.getName() == "TESWorldSpace":
            matches.append(data_type)
    if not matches:
        print("<TESWorldSpace data type not found>")
        return
    for data_type in matches:
        print(f"{data_type.getPathName()} length=0x{data_type.getLength():x}")
        if not hasattr(data_type, "getComponents"):
            continue
        for component in data_type.getComponents():
            offset = component.getOffset()
            if 0x120 <= offset <= 0x178:
                print(
                    f"  +0x{offset:03x} {component.getFieldName()} "
                    f"{component.getDataType().getDisplayName()}"
                )


def function_for_rva(program, rva):
    address = program.getImageBase().add(rva)
    manager = program.getFunctionManager()
    return address, (
        manager.getFunctionAt(address)
        or manager.getFunctionContaining(address)
    )


def print_named_matches(program, limit=250):
    print("\n-- Relevant named functions --")
    count = 0
    for function in program.getFunctionManager().getFunctions(True):
        name = function.getName(True)
        if not NAME_PATTERN.search(name):
            continue
        print(f"0x{function.getEntryPoint().getOffset():x} {name}")
        count += 1
        if count >= limit:
            print(f"<truncated after {limit}>")
            break
    if count == 0:
        print("<none>")


def print_target(program, interface, monitor, label, rva):
    address, function = function_for_rva(program, rva)
    function_name = function.getName(True) if function else "<none>"
    print(
        f"\n-- {label} RVA=0x{rva:x} VA={address} "
        f"function={function_name} --"
    )
    print(decompile(interface, function, monitor))

    callers = {}
    for reference in program.getReferenceManager().getReferencesTo(address):
        caller = program.getFunctionManager().getFunctionContaining(
            reference.getFromAddress()
        )
        if caller is not None:
            callers[caller.getEntryPoint().getOffset()] = caller
    print(f"-- {label} direct referencing functions ({len(callers)}) --")
    for entry, caller in sorted(callers.items()):
        print(f"0x{entry:x} {caller.getName(True)}")


def operand_value_matches_0x148(obj):
    try:
        unsigned = int(obj.getUnsignedValue())
        signed = int(obj.getSignedValue())
    except Exception:
        return False
    return unsigned == 0x148 or signed == 0x148


def find_0x148_functions(program):
    found = {}
    listing = program.getListing()
    manager = program.getFunctionManager()
    for instruction in listing.getInstructions(True):
        hit = False
        for operand_index in range(instruction.getNumOperands()):
            for obj in instruction.getOpObjects(operand_index):
                if operand_value_matches_0x148(obj):
                    hit = True
                    break
            if hit:
                break
        if not hit:
            continue
        function = manager.getFunctionContaining(instruction.getAddress())
        if function is None:
            continue
        entry = function.getEntryPoint().getOffset()
        record = found.setdefault(entry, {"function": function, "instructions": []})
        if len(record["instructions"]) < 8:
            record["instructions"].append(
                f"{instruction.getAddress()} {instruction}"
            )
    return found


def print_0x148_candidates(program, interface, monitor, decompile_limit=20):
    candidates = find_0x148_functions(program)
    print(
        "\n-- Functions with scalar instruction operand 0x148 "
        f"({len(candidates)}) --"
    )
    shown = 0
    for entry, record in sorted(candidates.items()):
        function = record["function"]
        text = decompile(interface, function, monitor, timeout=20)
        lower = text.lower()
        interesting = (
            "teleport" in lower
            or "door" in lower
            or "worldspace" in lower
            or "+ 0x148" in lower
            or "+0x148" in lower
        )
        if not interesting:
            continue
        print(
            f"\n0x{entry:x} {function.getName(True)} "
            f"operands={len(record['instructions'])}"
        )
        for instruction in record["instructions"]:
            print(f"  {instruction}")
        print(text)
        shown += 1
        if shown >= decompile_limit:
            print(f"<interesting decompilations truncated after {decompile_limit}>")
            break
    if shown == 0:
        print("<no decompilation contained a teleport/door/worldspace marker>")


def inspect_program(
    project,
    monitor,
    tag,
    config,
    scan_0x148,
    quick,
    requested_target,
):
    import java.lang
    from ghidra.app.decompiler import DecompInterface

    domain_file = project.getProjectData().getFile(config["path"])
    if domain_file is None:
        print(f"\n=== {tag}: missing {config['path']} ===")
        return
    consumer = java.lang.Object()
    program = domain_file.getDomainObject(consumer, False, False, monitor)
    interface = DecompInterface()
    try:
        interface.openProgram(program)
        print(
            f"\n================ {tag} {program.getName()} "
            f"base={program.getImageBase()} ================"
        )
        if not quick:
            print_worldspace_layout(program)
            print_named_matches(program)
        for label, rva in config["rvas"].items():
            if requested_target and label != requested_target:
                continue
            print_target(program, interface, monitor, label, rva)
        if scan_0x148:
            print_0x148_candidates(program, interface, monitor)
    finally:
        interface.dispose()
        program.release(consumer)


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)
    os.environ.setdefault("GHIDRA_INSTALL_DIR", str(GHIDRA_DIR))
    import pyghidra

    pyghidra.start(install_dir=GHIDRA_DIR)
    from ghidra.util.task import ConsoleTaskMonitor

    monitor = ConsoleTaskMonitor()
    scan_0x148 = "--scan-0x148" in sys.argv[1:]
    quick = "--quick" in sys.argv[1:]
    requested_target = None
    for arg in sys.argv[1:]:
        if arg.startswith("--target="):
            requested_target = arg.split("=", 1)[1]
    requested = {
        arg.upper()
        for arg in sys.argv[1:]
        if not arg.startswith("--")
    }
    unknown = requested.difference(PROGRAMS)
    if unknown:
        raise SystemExit(
            "Unknown program tag(s): " + ", ".join(sorted(unknown))
        )
    with pyghidra.open_project(
        PROJECT_DIR, PROJECT_NAME, create=False
    ) as project:
        for tag, config in PROGRAMS.items():
            if requested and tag not in requested:
                continue
            inspect_program(
                project,
                monitor,
                tag,
                config,
                scan_0x148,
                quick,
                requested_target,
            )


if __name__ == "__main__":
    main()
