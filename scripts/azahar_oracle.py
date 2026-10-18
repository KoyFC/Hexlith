"""Running cases through Azahar's own code, for the comparison scripts.

third_party/azahar/oracle.patch makes Azahar, when HEXLITH_ORACLE_DIR names a
directory, run the case files in it at its first vertical blank, write the
results beside them and a done.txt, and quit. It needs a title to boot to get
there, so it is given the cartridge image. Its configuration and data go in
the directory too, so that a person's Azahar profile is neither read nor
written.
"""

import os
import subprocess
import sys


def run_azahar(azahar, image, work, timeout):
    """Runs the case files in `work` through Azahar, or stops the script."""
    environment = dict(os.environ)
    environment["HEXLITH_ORACLE_DIR"] = str(work)
    environment["XDG_CONFIG_HOME"] = str(work / "config")
    environment["XDG_DATA_HOME"] = str(work / "data")
    (work / "done.txt").unlink(missing_ok=True)
    subprocess.run([azahar, image], env=environment, timeout=timeout,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    if not (work / "done.txt").exists():
        sys.exit("Azahar quit without running the cases")


def clear(work, names):
    """Removes case and result files a previous run left in `work`."""
    for name in names:
        (work / name).unlink(missing_ok=True)


# Every case file the oracle knows, so that each script can leave only its own.
CASE_FILES = ("y2r-cases.bin", "pica-cases.bin", "shader-cases.bin", "texture-cases.bin",
              "raster-cases.bin")
