"""
Cross-check the kernel bridge's ordinal routing against the export table.

Run: py -3 tools/kernel_audit/test_bridge_ordinals.py

src/kernel/kernel_bridge.c maps ordinals to bridge_<Name> wrappers by hand.
Nothing tied those numbers to the real kernel export list, and a block of them
had drifted by one -- so ordinal 24 (ExQueryNonVolatileSetting) dispatched to
ExQueryPoolBlockSize, and 67 (IoCreateSymbolicLink) to IoCreateFile. Both
"worked" in that they returned a plausible value, which is what made it hard
to see: the game got a pool size where it wanted EEPROM settings, and a
file-not-found where it wanted a drive mount.

KERNEL_EXPORTS in tools/xbe_parser is the reference. It is ordinal-indexed and
matches the published Xbox kernel export table.
"""

import os
import re
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
sys.path.insert(0, ROOT)

BRIDGE_C = os.path.join(ROOT, "src", "kernel", "kernel_bridge.c")
PARSER_PY = os.path.join(ROOT, "tools", "xbe_parser", "xbe_parser.py")


ABI_INC = os.path.join(ROOT, "src", "kernel", "kernel_abi.inc")


def load_abi():
    """ordinal -> (convention, stack bytes popped, register arguments), from the table nxdk's prototypes generate."""
    rows = re.findall(r"\{ KABI_(\w+), (\d+), (\d+) \},\s*/\* (\d+)", open(ABI_INC, encoding="utf-8").read())
    assert len(rows) > 300, f"only {len(rows)} rows in kernel_abi.inc"
    return {int(o): (conv, int(stack), int(regs)) for conv, stack, regs, o in rows}


def load_exports():
    with open(PARSER_PY, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    m = re.search(r"KERNEL_EXPORTS\s*[:=][^{]*\{(.*?)\n\}", src, re.S)
    assert m, "could not locate KERNEL_EXPORTS in tools/xbe_parser"
    pairs = re.findall(r"(\d+)\s*:\s*[\"']([A-Za-z_][A-Za-z0-9_]*)", m.group(1))
    exports = {int(o): n for o, n in pairs}
    assert len(exports) > 300, f"only parsed {len(exports)} exports"
    return exports


def load_routes():
    with open(BRIDGE_C, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    routes = re.findall(
        r"case\s+(\d+):\s*return\s+bridge_([A-Za-z_][A-Za-z0-9_]*)\s*;", src)
    assert routes, "no bridge routes found"
    return [(int(o), n) for o, n in routes]


def test_every_route_matches_its_ordinal():
    exports = load_exports()
    bad = []
    for ordinal, name in load_routes():
        expected = exports.get(ordinal)
        if expected is None:
            bad.append(f"ordinal {ordinal} -> bridge_{name}, but no such export")
        elif expected != name:
            bad.append(
                f"ordinal {ordinal} -> bridge_{name}, but ordinal {ordinal} "
                f"is {expected}")

    assert not bad, "misrouted kernel ordinals:\n  " + "\n  ".join(bad)
    print(f"ok  every_route_matches_its_ordinal ({len(load_routes())} routes)")


def test_no_duplicate_ordinals():
    seen, dupes = set(), []
    for ordinal, name in load_routes():
        if ordinal in seen:
            dupes.append(f"ordinal {ordinal} routed twice (last: bridge_{name})")
        seen.add(ordinal)
    assert not dupes, "\n  ".join(dupes)
    print("ok  no_duplicate_ordinals")


def test_every_routed_ordinal_is_a_function_of_that_name():
    """Each bridged ordinal is a function in kernel_abi.inc under the bridge's name.

    The dispatcher pops what the table says, so a bridge on an ordinal the
    table calls data or does not know pops nothing and leaks its arguments, as
    ordinal 47 (HalRegisterShutdownNotification) once did when its hand-written
    size was forgotten: esp came back 8 bytes low and the caller's epilogue
    restored ebx/esi/edi from the wrong slots.
    """
    names = dict(re.findall(r"\{ KABI_\w+, \d+, \d+ \},\s*/\* (\d+) (\w+)",
                            open(ABI_INC, encoding="utf-8").read()))
    abi = load_abi()
    bad = []
    for ordinal, name in load_routes():
        kind = abi[ordinal][0]
        if kind == "DATA":
            continue
        if kind not in ("STDCALL", "FASTCALL", "CDECL"):
            bad.append(f"ordinal {ordinal} routes to bridge_{name}, but kernel_abi.inc says {kind}")
        elif names.get(str(ordinal)) != name:
            bad.append(f"ordinal {ordinal} routes to bridge_{name}, but kernel_abi.inc names it {names.get(str(ordinal))}")
    assert not bad, "\n  ".join(bad)
    print(f"ok  every_routed_ordinal_is_a_function_of_that_name ({len(load_routes())} routes)")


def load_data_exports():
    """(ordinal, KDATA slot) for every entry in kernel_data_va_for_ordinal."""
    src = open(BRIDGE_C, encoding="utf-8", errors="replace").read()
    m = re.search(r"kernel_data_va_for_ordinal.*?\n\}", src, re.S)
    assert m, "could not locate kernel_data_va_for_ordinal"
    return [(int(o), k) for o, k in re.findall(
        r"case\s+(\d+):\s*return\s+XBOX_KERNEL_DATA_BASE\s*\+\s*(KDATA_[A-Z_0-9]+)",
        m.group(0))]


# Which export each KDATA slot is the storage for. The slot names are ours, so
# the mapping cannot be derived from the export table and has to be stated.
KDATA_OWNER = {
    "KDATA_EVENT_OBJ_TYPE": "ExEventObjectType",
    "KDATA_MUTANT_OBJ_TYPE": "ExMutantObjectType",
    "KDATA_SEMAPHORE_OBJ_TYPE": "ExSemaphoreObjectType",
    "KDATA_TIMER_OBJ_TYPE": "ExTimerObjectType",
    "KDATA_DISK_MODEL_STR": "HalDiskModelNumber",
    "KDATA_DISK_SERIAL_STR": "HalDiskSerialNumber",
    "KDATA_DISK_CACHE_PARTS": "HalDiskCachePartitionCount",
    "KDATA_IO_COMPLETION_TYPE": "IoCompletionObjectType",
    "KDATA_IO_DEVICE_TYPE": "IoDeviceObjectType",
    "KDATA_FILE_OBJ_TYPE": "IoFileObjectType",
    "KDATA_TICK_COUNT": "KeTickCount",
    "KDATA_KD_DEBUGGER_ENABLED": "KdDebuggerEnabled",
    "KDATA_KD_DEBUGGER_NOT_PRESENT": "KdDebuggerNotPresent",
    "KDATA_TIME_INCREMENT": "KeTimeIncrement",
    "KDATA_LAUNCH_DATA_PAGE": "LaunchDataPage",
    "KDATA_THREAD_OBJ_TYPE": "PsThreadObjectType",
    "KDATA_HARDWARE_INFO": "XboxHardwareInfo",
    "KDATA_HD_KEY": "XboxHDKey",
    "KDATA_KRNL_VERSION": "XboxKrnlVersion",
    "KDATA_SIGNATURE_KEY": "XboxSignatureKey",
    "KDATA_XE_IMAGE_FILENAME": "XeImageFileName",
    "KDATA_LAN_KEY": "XboxLANKey",
    "KDATA_ALT_SIGNATURE_KEYS": "XboxAlternateSignatureKeys",
    "KDATA_XE_PUBLIC_KEY": "XePublicKeyData",
    "KDATA_BOOT_SMC_VIDEO": "HalBootSMCVideoMode",
    "KDATA_IDEX_CHANNEL": "IdexChannelObject",
}


def test_data_exports_match_their_ordinal():
    """kernel_data_va_for_ordinal is consulted BEFORE function routing, so a
    wrong ordinal here does not merely mis-read a variable -- it hands the title
    a data address for a real kernel function, which it then calls. A whole
    block of this table had shifted: 17 (ExFreePool), 65 (IoCreateDevice), 327
    (XeLoadSection) and 328 (XeUnloadSection) were all being turned into data
    pointers, and the real exports at 16/353/354/355/356/357 got no thunk."""
    exports = load_exports()
    entries = load_data_exports()
    assert entries, "no data-export entries found"
    bad = []
    for ordinal, slot in entries:
        actual = exports.get(ordinal)
        owner = KDATA_OWNER.get(slot)
        if actual is None:
            bad.append(f"ordinal {ordinal} -> {slot}, but no such export")
        elif owner is None:
            bad.append(f"{slot} has no entry in KDATA_OWNER (add one)")
        elif owner != actual:
            bad.append(f"ordinal {ordinal} -> {slot} ({owner}), but ordinal "
                       f"{ordinal} is {actual}")
    assert not bad, "misrouted kernel data exports:\n  " + "\n  ".join(bad)
    print(f"ok  data_exports_match_their_ordinal ({len(entries)} exports)")


def test_no_data_export_ordinal_is_also_a_function_route():
    """The two tables must not overlap: data wins at thunk-build time, so an
    ordinal in both is a function the title can never actually call."""
    data = {o for o, _ in load_data_exports()}
    both = sorted(data & {o for o, _ in load_routes()})
    assert not both, ("ordinals routed as BOTH data and function: %s" % both)
    print("ok  no_data_export_ordinal_is_also_a_function_route")

def bridge_bodies():
    src = open(BRIDGE_C, encoding="utf-8", errors="replace").read()
    out = {}
    for m in re.finditer(r"static void (bridge_\w+)\(void\)\s*\{", src):
        depth, i = 1, m.end()
        while depth:
            depth += {"{": 1, "}": -1}.get(src[i], 0)
            i += 1
        out[m.group(1)[len("bridge_"):]] = re.sub(r"/\*.*?\*/|//[^\n]*", "", src[m.end():i - 1], flags=re.S)
    return out


def test_bridges_read_the_arguments_their_prototype_has():
    """Every bridge reads its arguments where the Xbox kernel's prototype puts them.

    The dispatcher pops what kernel_abi.inc says, so a pop count can no longer
    disagree with nxdk. A bridge body still can: one written against NT's
    argument list reads the wrong slots, and one that takes a fastcall's
    argument from the stack reads the caller's locals. A stack-argument bridge
    reads its last argument, as `(void)STACK_ARG(n)` when it has no use for it,
    so the highest slot it names is the prototype's own count. eax is never an
    argument: InterlockedCompareExchange once took its Comparand from eax,
    where the caller had left whatever it last computed.
    """
    abi, bodies = load_abi(), bridge_bodies()
    bad = []
    for ordinal, name in load_routes():
        conv, stack, regs = abi[ordinal]
        body = bodies.get(name)
        if body is None or conv not in ("STDCALL", "FASTCALL"):
            continue
        slots = [int(n) for n in re.findall(r"\bSTACK_ARG\s*\(\s*(\d+)\s*\)", body)]
        if re.search(r"\bSTACK_ARG\s*\(\s*[^\d\s]", body):
            bad.append(f"{ordinal} {name}: STACK_ARG with a computed slot; name each slot")
        reads_ecx = re.search(r"\bg_ecx\b(?!\s*=[^=])", body)
        reads_edx = re.search(r"\bg_edx\b(?!\s*=[^=])", body)
        if slots and max(slots) >= stack // 4:
            bad.append(f"{ordinal} {name}: reads STACK_ARG({max(slots)}), but the prototype pushes {stack // 4}")
        elif stack and max(slots, default=-1) != stack // 4 - 1:
            bad.append(f"{ordinal} {name}: highest slot read is {max(slots, default='none')}, "
                       f"the prototype pushes {stack // 4}")
        first_eax = re.search(r"\bg_eax\b(\s*=[^=])?", body)
        if first_eax and not first_eax.group(1):
            bad.append(f"{ordinal} {name}: reads g_eax before setting it; eax carries no argument")
        if conv == "STDCALL" and (reads_ecx or reads_edx):
            bad.append(f"{ordinal} {name}: stdcall, but reads g_ecx/g_edx as an argument")
        if conv == "FASTCALL" and regs >= 1 and not reads_ecx:
            bad.append(f"{ordinal} {name}: fastcall, but never reads its first argument from g_ecx")
        if conv == "FASTCALL" and regs == 2 and not reads_edx:
            bad.append(f"{ordinal} {name}: fastcall, but never reads its second argument from g_edx")
    assert not bad, "bridges disagree with kernel_abi.inc:\n  " + "\n  ".join(bad)
    print("ok  bridges_read_the_arguments_their_prototype_has")


if __name__ == "__main__":
    # Discovered rather than listed: this file has been appended to before, and
    # a test defined after a hand-written call list is silently never run.
    _tests = [v for k, v in sorted(globals().items())
              if k.startswith("test_") and callable(v)]
    for _t in _tests:
        _t()
    print("all passed (%d checks)" % len(_tests))
