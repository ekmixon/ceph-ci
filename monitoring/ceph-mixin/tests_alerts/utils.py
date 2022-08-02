import pytest
import shutil
import subprocess


def promtool_available() -> bool:
    return shutil.which('promtool') is not None


def call(cmd):
    return subprocess.run(cmd.split(), stdout=subprocess.PIPE)
