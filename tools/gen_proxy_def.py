"""Generate a .def file that forwards every export of the system opengl32.dll
to opengl32_real.dll, except the handful we implement ourselves."""
import sys, pefile

SYS = r"C:\Windows\SysWOW64\opengl32.dll"
SKIP = set(sys.argv[1].split(",")) if len(sys.argv) > 1 and sys.argv[1] else set()

pe = pefile.PE(SYS, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
names = [e.name.decode() for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name]
pe.close()

DECOR = {"wglCreateContext": 4, "wglMakeCurrent": 8, "wglSwapBuffers": 4,
         "glViewport": 16, "glScissor": 16, "glOrtho": 48, "glLineWidth": 4}

lines = ["LIBRARY opengl32", "EXPORTS"]
n = 0
for name in names:
    if name in SKIP:
        continue
    lines.append(f"    {name}=opengl32_real.{name}")
    n += 1
for name in SKIP:
    if name in DECOR:
        lines.append(f"    {name}=_{name}@{DECOR[name]}")

print(f"forwarded {n} exports, implemented locally {len(SKIP)}")
sys.stdout.write("\n".join(lines) + "\n")
