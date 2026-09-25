#!/usr/bin/env python3
"""Read-only queries for Fallout 4 preload functions in the local Ghidra project.

This intentionally never starts a transaction and opens the program read-only.
It is a small investigation aid rather than part of the runtime plugin.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path


DEFAULT_GHIDRA_REPO = Path(r"C:\Development\Tools\BethesdaGhidraScripts")
DEFAULT_PROJECT_DIR = DEFAULT_GHIDRA_REPO / "ghidraprojects" / "BethesdaGhidraScripts"


def walk_files(folder, prefix=""):
    for domain_file in folder.getFiles():
        yield f"{prefix}/{domain_file.getName()}", domain_file
    for child in folder.getFolders():
        yield from walk_files(child, f"{prefix}/{child.getName()}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-dir", default=str(DEFAULT_PROJECT_DIR))
    parser.add_argument("--project-name", default="BethesdaGhidraScripts")
    parser.add_argument("--program-path", default="/f4/og/Fallout4.exe")
    parser.add_argument(
        "--pattern",
        action="append",
        default=[],
        help="Case-insensitive function/symbol-name substring (repeatable)",
    )
    parser.add_argument(
        "--rva",
        action="append",
        default=[],
        help="Function RVA to decompile, for example 0xfc310 (repeatable)",
    )
    parser.add_argument(
        "--string",
        action="append",
        default=[],
        help="Case-insensitive substring in a defined string; include its xref functions",
    )
    parser.add_argument(
        "--around-rva",
        action="append",
        default=[],
        help="List functions beginning within +/- --around-bytes of this RVA",
    )
    parser.add_argument(
        "--xref-rva",
        action="append",
        default=[],
        help="List references to an RVA and include containing functions",
    )
    parser.add_argument("--around-bytes", type=lambda value: int(value, 0), default=0x400)
    parser.add_argument("--decompile-matches", action="store_true")
    parser.add_argument("--max-matches", type=int, default=200)
    args = parser.parse_args()

    ghidra_dir = DEFAULT_GHIDRA_REPO / "tools" / "ghidra"
    os.environ.setdefault("GHIDRA_INSTALL_DIR", str(ghidra_dir))

    import pyghidra

    pyghidra.start(install_dir=ghidra_dir)
    import java.lang
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import ConsoleTaskMonitor
    monitor = ConsoleTaskMonitor()

    with pyghidra.open_project(
        args.project_dir, args.project_name, create=False
    ) as project:
        domain_file = None
        for path, candidate in walk_files(project.getProjectData().getRootFolder()):
            if path == args.program_path:
                domain_file = candidate
                break
        if domain_file is None:
            raise SystemExit(f"program not found: {args.program_path}")

        consumer = java.lang.Object()
        program = domain_file.getDomainObject(consumer, False, False, monitor)
        try:
            image_base = program.getImageBase()
            function_manager = program.getFunctionManager()
            symbol_table = program.getSymbolTable()
            patterns = [value.casefold() for value in args.pattern]
            string_patterns = [value.casefold() for value in args.string]

            matches = {}
            if patterns:
                for function in function_manager.getFunctions(True):
                    haystack = (
                        f"{function.getName()} "
                        f"{function.getName(True)}"
                    ).casefold()
                    if any(pattern in haystack for pattern in patterns):
                        matches[int(function.getEntryPoint().getOffset())] = function

                for symbol in symbol_table.getAllSymbols(True):
                    name = str(symbol.getName(True))
                    if any(pattern in name.casefold() for pattern in patterns):
                        function = function_manager.getFunctionContaining(
                            symbol.getAddress()
                        )
                        if function is not None:
                            matches[int(function.getEntryPoint().getOffset())] = function
                        elif len(matches) < args.max_matches:
                            address = symbol.getAddress()
                            rva = address.subtract(image_base)
                            print(f"SYMBOL RVA {int(rva):#x} {name}")

            if string_patterns:
                reference_manager = program.getReferenceManager()
                listing = program.getListing()
                for data in listing.getDefinedData(True):
                    try:
                        value = data.getValue()
                        rendered = str(value)
                    except Exception:
                        continue
                    if not any(pattern in rendered.casefold() for pattern in string_patterns):
                        continue
                    data_rva = data.getAddress().subtract(image_base)
                    print(f"STRING RVA {int(data_rva):#x} {rendered!r}")
                    for reference in reference_manager.getReferencesTo(data.getAddress()):
                        source = reference.getFromAddress()
                        source_rva = source.subtract(image_base)
                        function = function_manager.getFunctionContaining(source)
                        function_name = (
                            function.getName(True) if function is not None else "<no function>"
                        )
                        print(
                            f"  XREF RVA {int(source_rva):#x} "
                            f"type={reference.getReferenceType()} {function_name}"
                        )
                        if function is not None:
                            matches[int(function.getEntryPoint().getOffset())] = function

            for raw_center in args.around_rva:
                center = int(raw_center, 0)
                lower = center - args.around_bytes
                upper = center + args.around_bytes
                print(
                    f"FUNCTIONS AROUND RVA {center:#x} "
                    f"(+/- {args.around_bytes:#x})"
                )
                for function in function_manager.getFunctions(True):
                    rva = int(function.getEntryPoint().subtract(image_base))
                    if lower <= rva <= upper:
                        print(
                            f"  RVA {rva:#x} {function.getName(True)} "
                            f"size={int(function.getBody().getNumAddresses()):#x}"
                        )
                        matches[int(function.getEntryPoint().getOffset())] = function

            for raw_target in args.xref_rva:
                target_rva = int(raw_target, 0)
                target = image_base.add(target_rva)
                print(f"XREFS TO RVA {target_rva:#x}")
                for reference in program.getReferenceManager().getReferencesTo(target):
                    source = reference.getFromAddress()
                    source_rva = source.subtract(image_base)
                    function = function_manager.getFunctionContaining(source)
                    function_name = (
                        function.getName(True) if function is not None else "<no function>"
                    )
                    print(
                        f"  RVA {int(source_rva):#x} "
                        f"type={reference.getReferenceType()} {function_name}"
                    )
                    if function is not None:
                        matches[int(function.getEntryPoint().getOffset())] = function

            for raw_rva in args.rva:
                address = image_base.add(int(raw_rva, 0))
                function = function_manager.getFunctionContaining(address)
                if function is None:
                    print(f"NO FUNCTION CONTAINING RVA {raw_rva}")
                    continue
                matches[int(function.getEntryPoint().getOffset())] = function

            ordered = sorted(
                matches.values(), key=lambda value: value.getEntryPoint().getOffset()
            )[: args.max_matches]
            for function in ordered:
                rva = function.getEntryPoint().subtract(image_base)
                print(
                    f"FUNCTION RVA {int(rva):#x} "
                    f"{function.getName(True)} "
                    f"size={int(function.getBody().getNumAddresses()):#x}"
                )

            should_decompile = bool(args.rva) or args.decompile_matches
            if should_decompile and ordered:
                decompiler = DecompInterface()
                decompiler.openProgram(program)
                try:
                    for function in ordered:
                        rva = function.getEntryPoint().subtract(image_base)
                        print(f"\n===== DECOMPILE RVA {int(rva):#x} =====")
                        result = decompiler.decompileFunction(function, 60, monitor)
                        if result and result.decompileCompleted():
                            decompiled = result.getDecompiledFunction()
                            print(decompiled.getC() if decompiled else "<no C output>")
                        else:
                            print(
                                result.getErrorMessage()
                                if result
                                else "<decompiler returned no result>"
                            )
                finally:
                    decompiler.dispose()
        finally:
            program.release(consumer)


if __name__ == "__main__":
    main()
