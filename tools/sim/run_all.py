#!/usr/bin/env python3
"""Unified L0 (static), L1 (host Unity tests), L2 (contract), and L3 (Playwright UI) runner."""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(THIS_DIR, "..", ".."))
if THIS_DIR not in sys.path:
    sys.path.insert(0, THIS_DIR)

import mock_device  # noqa: E402
import test_contract  # noqa: E402

ALL_LAYERS = ("L0", "L1", "L2", "L3")

_SKIP_DIRS = {
    ".git",
    ".pio",
    "__pycache__",
    ".pytest_cache",
    "node_modules",
    "build",
    "dist",
    ".venv",
    "venv",
}

_BINARY_EXTS = {
    ".stl", ".step", ".stp", ".sldprt", ".sldasm",
    ".png", ".jpg", ".jpeg", ".gif", ".ico", ".pdf",
    ".bin", ".elf", ".o", ".a", ".pyc", ".pyd",
    ".so", ".dll", ".exe", ".zip", ".7z", ".gz",
    ".woff", ".woff2", ".ttf",
}


def _extract_web_ui_script():
    with open(mock_device.WEB_UI_PATH, "r", encoding="utf-8") as f:
        raw = f.read()
    count_end = raw.count(')rawliteral"')
    if count_end != 1:
        return None, f'expected exactly 1 )rawliteral" marker, found {count_end}'
    try:
        html = mock_device.extract_web_ui_html()
    except RuntimeError as exc:
        return None, str(exc)
    sm = re.search(r"<script>(.*?)</script>", html, re.DOTALL)
    if not sm:
        return None, "missing <script> block in web_ui.h"
    return sm.group(1), ""


def check_l0_pio_build():
    pio_exe, pio_err = test_contract.find_pio()
    if not pio_exe:
        return False, pio_err

    fw_dir = os.path.join(REPO_ROOT, "firmware")
    try:
        clean_proc = subprocess.run(
            [pio_exe, "run", "-d", fw_dir, "-e", "esp32dev", "-t", "clean"],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:
        return False, f"failed to launch pio clean: {exc}"

    if clean_proc.returncode != 0:
        tail = (clean_proc.stderr or clean_proc.stdout).strip().splitlines()[-3:]
        return False, f"pio clean failed ({clean_proc.returncode}): {' | '.join(tail)}"

    try:
        proc = subprocess.run(
            [pio_exe, "run", "-d", fw_dir, "-e", "esp32dev"],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:
        return False, f"failed to launch pio build: {exc}"

    if proc.returncode != 0:
        tail = (proc.stderr or proc.stdout).strip().splitlines()[-3:]
        return False, f"pio run -e esp32dev failed ({proc.returncode}): {' | '.join(tail)}"

    src_warnings = [
        ln for ln in (proc.stdout + "\n" + proc.stderr).splitlines()
        if re.search(r"(^|[\\/:\s])src[\\/].*warning:", ln, re.IGNORECASE)
    ]
    if src_warnings:
        return False, f"compiler warnings in src: {src_warnings[0]}"
    return True, "esp32dev clean build passed (0 src warnings)"


def check_l0_webui_syntax():
    js, err = _extract_web_ui_script()
    if js is None:
        return False, err

    try:
        proc = subprocess.run(
            ["node", "--check", "-"],
            input=js,
            capture_output=True,
            text=True,
            check=False,
        )
    except FileNotFoundError:
        return False, "node not found in PATH; cannot run node --check"
    except OSError as exc:
        return False, f"failed to launch node: {exc}"

    if proc.returncode != 0:
        return False, f"node --check failed: {proc.stderr.strip()}"
    return True, 'single )rawliteral" marker and JS passes node --check'


def _strip_js_strings_and_comments(js):
    """Replace JS comments, quoted strings, and template text with spaces, keeping ${...} code."""
    out = []
    i = 0
    n = len(js)
    template_expr_depths = []
    brace_depth = 0

    while i < n:
        ch = js[i]
        if ch == "/" and i + 1 < n and js[i + 1] == "/":
            nl = js.find("\n", i)
            end = n if nl == -1 else nl
            out.append(" " * (end - i))
            i = end
            continue
        if ch == "/" and i + 1 < n and js[i + 1] == "*":
            end = js.find("*/", i + 2)
            end = n if end == -1 else end + 2
            out.append(" " * (end - i))
            i = end
            continue
        if ch in ('"', "'"):
            quote = ch
            out.append(" ")
            i += 1
            while i < n:
                out.append(" ")
                if js[i] == "\\" and i + 1 < n:
                    out.append(" ")
                    i += 2
                    continue
                if js[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if ch == "`":
            out.append(" ")
            i += 1
            while i < n:
                if js[i] == "\\" and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                if js[i] == "`":
                    out.append(" ")
                    i += 1
                    break
                if js[i] == "$" and i + 1 < n and js[i + 1] == "{":
                    out.append("  ")
                    brace_depth += 1
                    template_expr_depths.append(brace_depth)
                    i += 2
                    break
                out.append("\n" if js[i] == "\n" else " ")
                i += 1
            continue

        if ch == "{":
            brace_depth += 1
            out.append(ch)
            i += 1
            continue
        if ch == "}":
            if template_expr_depths and brace_depth == template_expr_depths[-1]:
                template_expr_depths.pop()
                brace_depth -= 1
                out.append(" ")
                i += 1
                # Resume scanning template string body
                while i < n:
                    if js[i] == "\\" and i + 1 < n:
                        out.append("  ")
                        i += 2
                        continue
                    if js[i] == "`":
                        out.append(" ")
                        i += 1
                        break
                    if js[i] == "$" and i + 1 < n and js[i + 1] == "{":
                        out.append("  ")
                        brace_depth += 1
                        template_expr_depths.append(brace_depth)
                        i += 2
                        break
                    out.append("\n" if js[i] == "\n" else " ")
                    i += 1
                continue
            brace_depth = max(0, brace_depth - 1)
            out.append(ch)
            i += 1
            continue

        out.append(ch)
        i += 1

    return "".join(out)


def _has_object_spread(code):
    """Detect ES2018 object spread ({ ...obj }) while allowing ES2015 array/call/rest spread."""
    stack = []
    i = 0
    n = len(code)
    while i < n:
        ch = code[i]
        if ch in ("{", "[", "("):
            if ch == "{":
                # Determine whether '{' opens a block/function or an object literal
                j = i - 1
                while j >= 0 and code[j].isspace():
                    j -= 1
                prev = code[j] if j >= 0 else ""
                is_obj = prev in ("", "=", ":", ",", "(", "[", "?", "!", "&", "|", ";") or code[max(0, j - 5):j + 1].endswith("return")
                stack.append("{" if is_obj else "block")
            else:
                stack.append(ch)
            i += 1
            continue
        if ch in ("}", "]", ")"):
            if stack:
                stack.pop()
            i += 1
            continue
        if code.startswith("...", i) and stack and stack[-1] == "{":
            return True
        i += 1
    return False


def check_l0_webui_es2017():
    js, err = _extract_web_ui_script()
    if js is None:
        return False, err

    code = _strip_js_strings_and_comments(js)
    violations = []
    if re.search(r"(&&=|\|\|=|\?\?=)", code):
        violations.append("logical assignment (&&=/||=/??=)")
    if "??" in code.replace("??=", ""):
        violations.append("nullish coalescing (??)")
    if re.search(r"\?\.[A-Za-z_$(\[]", code):
        violations.append("optional chaining (?.)")
    if _has_object_spread(code):
        violations.append("object spread ({...x})")
    if re.search(r"\bcatch\s*\{", code):
        violations.append("optional catch binding (catch {)")
    if re.search(r"\.replaceAll\s*\(", code):
        violations.append("String.prototype.replaceAll")

    if violations:
        return False, f"ES2017 ceiling violation(s) in web_ui JS: {', '.join(violations)}"
    return True, "web_ui JS within ES2017 ceiling"


def _repo_files():
    """Files the repository tracks; a plain directory walk when git is not available."""
    try:
        out = subprocess.run(["git", "ls-files", "-z"], cwd=REPO_ROOT, capture_output=True, check=True).stdout
        return sorted(os.path.join(REPO_ROOT, p) for p in out.decode("utf-8").split("\0") if p)
    except (OSError, subprocess.CalledProcessError):
        paths = []
        for root, dirs, files in os.walk(REPO_ROOT):
            dirs[:] = sorted(dn for dn in dirs if dn not in _SKIP_DIRS)
            paths.extend(os.path.join(root, fn) for fn in sorted(files))
        return paths


def check_l0_no_em_dash():
    hits = []
    for p in _repo_files():
        if os.path.splitext(p)[1].lower() in _BINARY_EXTS:
            continue
        try:
            with open(p, "rb") as bf:
                head = bf.read(4096)
            if b"\x00" in head:
                continue
            with open(p, "r", encoding="utf-8", errors="ignore") as tf:
                if "\u2014" in tf.read():
                    hits.append(os.path.relpath(p, REPO_ROOT).replace("\\", "/"))
        except OSError:
            continue

    if hits:
        return False, f"U+2014 em dash found in: {', '.join(hits)}"
    return True, "0 U+2014 em-dash characters across repository text files"


def run_l1():
    pio_exe, pio_err = test_contract.find_pio()
    if not pio_exe:
        return [("L1_pio_test_native", "FAIL", pio_err)]

    fw_dir = os.path.join(REPO_ROOT, "firmware")
    try:
        proc = subprocess.run(
            [pio_exe, "test", "-e", "native", "-d", fw_dir],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:
        return [("L1_pio_test_native", "FAIL", f"failed to launch pio test: {exc}")]

    out = proc.stdout + "\n" + proc.stderr
    rows = []
    for ln in out.splitlines():
        m = re.match(r"^\s*native\s+(\S+)\s+(PASSED|FAILED|IGNORED|SKIPPED)\s+(\S+)", ln)
        if m:
            suite, status_tok, dur = m.group(1), m.group(2), m.group(3)
            st = "PASS" if status_tok == "PASSED" else "FAIL"
            rows.append((f"L1_{suite}", st, f"{status_tok.lower()} ({dur})"))

    total_cases = 0
    m_sum = re.search(r"(\d+)\s+test\s+cases?:\s+([^\n]+)", out)
    if m_sum:
        total_cases = int(m_sum.group(1))

    if proc.returncode != 0:
        if not any(st == "FAIL" for _, st, _ in rows):
            tail = " | ".join(out.strip().splitlines()[-3:])
            rows.append(("L1_pio_test_native", "FAIL", tail or f"exit {proc.returncode}"))
        return rows

    if total_cases == 0 or not rows:
        rows.append(("L1_pio_test_native", "FAIL", f"zero test cases parsed from pio test output (total={total_cases}, suites={len(rows)})"))
    return rows


def run_l2():
    pio_exe, pio_err = test_contract.find_pio()
    tmp_dir = tempfile.mkdtemp(prefix="pom_l2_")
    payload_exec_error = None
    try:
        if not pio_exe:
            payload_exec_error = pio_err
        else:
            fw_dir = os.path.join(REPO_ROOT, "firmware")
            cases_path = os.path.join(THIS_DIR, "config_cases.json")
            try:
                proc = subprocess.run(
                    [pio_exe, "run", "-d", fw_dir, "-e", "payloads", "-t", "exec", "-a", tmp_dir, "-a", cases_path],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                if proc.returncode != 0:
                    tail = " | ".join((proc.stderr or proc.stdout).strip().splitlines()[-3:])
                    payload_exec_error = f"pio run -e payloads -t exec failed ({proc.returncode}): {tail}"
            except OSError as exc:
                payload_exec_error = f"failed to launch pio payloads exec: {exc}"

        rows = []
        for name, errs in test_contract.run_all_checks(
            payload_dir=None if payload_exec_error else tmp_dir,
            payload_exec_error=payload_exec_error,
        ):
            rows.append((name, "PASS" if len(errs) == 0 else "FAIL", "; ".join(errs) if errs else "ok"))
        return rows
    finally:
        shutil.rmtree(tmp_dir, ignore_errors=True)


def run_l3():
    probe = subprocess.run(
        [sys.executable, "-c", "import playwright.sync_api, pytest"],
        capture_output=True,
        text=True,
        check=False,
    )
    if probe.returncode != 0:
        return [("L3_web_ui", "FAIL", f"pytest/playwright unavailable in {sys.executable}; install tools/sim/requirements.txt")]

    test_file = os.path.join(THIS_DIR, "test_web_ui.py")
    cmd = [
        sys.executable,
        "-m",
        "pytest",
        test_file,
        f"--rootdir={THIS_DIR}",
        "-p",
        "no:cacheprovider",
        "-v",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, env=os.environ.copy(), check=False)
    rows = []
    out = proc.stdout + "\n" + proc.stderr
    for ln in proc.stdout.splitlines():
        m = re.search(r"::(test_[A-Za-z0-9_]+)\s+(PASSED|FAILED|SKIPPED)", ln)
        if m:
            tname, outcome = m.group(1), m.group(2)
            st = "PASS" if outcome == "PASSED" else "FAIL"
            rows.append((f"L3_{tname}", st, "ok" if st == "PASS" else outcome.lower()))

    if proc.returncode != 0 and not any(st == "FAIL" for _, st, _ in rows):
        tail = " | ".join(out.strip().splitlines()[-3:])
        rows.append(("L3_pytest_runner", "FAIL", tail or f"exit {proc.returncode}"))
    elif not rows:
        rows.append(("L3_pytest_runner", "FAIL", "no test rows parsed from pytest output"))
    return rows


def _parse_layers(raw):
    requested = [tok.strip().upper() for tok in raw.split(",") if tok.strip()]
    if not requested:
        raise argparse.ArgumentTypeError("at least one layer must be specified")
    invalid = [tok for tok in requested if tok not in ALL_LAYERS]
    if invalid:
        raise argparse.ArgumentTypeError(f"unknown layer(s): {', '.join(invalid)} (valid: {', '.join(ALL_LAYERS)})")
    return set(requested)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Power Outage Monitor verification runner (L0-L3)")
    parser.add_argument(
        "--layers",
        type=_parse_layers,
        default=set(ALL_LAYERS),
        help="Comma-separated layers to run from L0,L1,L2,L3 (default: L0,L1,L2,L3)",
    )
    args = parser.parse_args(argv)
    selected = args.layers

    results = []

    if "L0" in selected:
        for name, fn in (
            ("L0_esp32dev_build", check_l0_pio_build),
            ("L0_webui_js_syntax_and_rawliteral", check_l0_webui_syntax),
            ("L0_webui_es2017_ceiling", check_l0_webui_es2017),
            ("L0_no_em_dash_u2014", check_l0_no_em_dash),
        ):
            ok, detail = fn()
            results.append((name, "PASS" if ok else "FAIL", detail))
    else:
        results.append(("L0_static", "SKIP", "not requested via --layers"))

    if "L1" in selected:
        results.extend(run_l1())
    else:
        results.append(("L1_native_unit_tests", "SKIP", "not requested via --layers"))

    if "L2" in selected:
        results.extend(run_l2())
    else:
        results.append(("L2_contract", "SKIP", "not requested via --layers"))

    if "L3" in selected:
        results.extend(run_l3())
    else:
        results.append(("L3_web_ui", "SKIP", "not requested via --layers"))

    passed = sum(1 for _, st, _ in results if st == "PASS")
    failed = sum(1 for _, st, _ in results if st == "FAIL")
    skipped = sum(1 for _, st, _ in results if st == "SKIP")

    print("\n=== Power Outage Monitor Verification Suite (L0-L3) ===")
    print(f"{'Test Name':<48} {'Result':<6} Detail")
    print("-" * 100)
    for name, status, detail in results:
        short_detail = detail if len(detail) <= 72 or status != "PASS" else detail[:69] + "..."
        print(f"{name:<48} {status:<6} {short_detail}")
    print("-" * 100)
    print(f"TOTAL: {passed} passed, {failed} failed, {skipped} skipped ({len(results)} checks)")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
