"""Keep potentially blocking native-driver work out of the desktop UI."""
import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys


async def run_worker(arguments, timeout=90):
    kwargs = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    process = await asyncio.create_subprocess_exec(
        sys.executable, str(Path(__file__).with_name("probe.py")), *arguments,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE, **kwargs)
    try:
        output, errors = await asyncio.wait_for(process.communicate(), timeout)
    except (asyncio.TimeoutError, asyncio.CancelledError) as exc:
        if process.returncode is None:
            process.kill()
        await process.communicate()
        if isinstance(exc, asyncio.CancelledError):
            raise
        return {"error": f"GPU worker exceeded {timeout} seconds and was stopped. No FSR result was established."}
    try:
        report = json.loads(output.decode("utf-8"))
    except (ValueError, UnicodeError):
        return {"error": f"GPU worker exited with code {process.returncode} without a valid report. "
                         + errors.decode("utf-8", "replace")[-2000:]}
    if process.returncode and not report.get("error") and not any(
        t["status"] == "failed" for t in report.get("tests", {}).values()
    ):
        report["error"] = f"GPU worker exited unexpectedly: {process.returncode}."
    if errors:
        report["worker_stderr"] = errors.decode("utf-8", "replace")[-6000:]
    return report
