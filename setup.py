import os
import shutil
import subprocess
import sysconfig

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

ROOT = os.path.dirname(os.path.abspath(__file__))


def build_tool():
    prebuilt = os.environ.get("MKC_PREBUILT")
    if prebuilt:
        return prebuilt
    env = dict(os.environ)
    env.setdefault("CC", (sysconfig.get_config_var("CC") or "cc").split()[0])
    if not os.path.exists(os.path.join(ROOT, "configure")):
        subprocess.run(["sh", "bootstrap"], cwd=ROOT, check=True, env=env)
    obj = os.path.join(ROOT, "build", "python-tool")
    os.makedirs(obj, exist_ok=True)
    if not os.path.exists(os.path.join(obj, "Makefile")):
        subprocess.run(["sh", os.path.join(ROOT, "configure")], cwd=obj,
                       check=True, env=env)
    subprocess.run(["make", "mkc"], cwd=obj, check=True, env=env)
    return os.path.join(obj, "mkc")


class BuildWithTool(build_ext):
    def run(self):
        super().run()
        tool = build_tool()
        targets = [os.path.join(self.build_lib, "mkc", "bin")]
        if self.inplace:
            targets.append(os.path.join(ROOT, "python", "mkc", "bin"))
        for target in targets:
            os.makedirs(target, exist_ok=True)
            shutil.copy2(tool, os.path.join(target, "mkc"))


setup(
    ext_modules=[Extension("mkc._mkc", ["python/mkc/_mkc.c"],
                           include_dirs=[os.path.join(ROOT, "src")],
                           depends=["src/mkc.h"],
                           extra_compile_args=["-O3", "-std=c99"])],
    cmdclass={"build_ext": BuildWithTool},
)
