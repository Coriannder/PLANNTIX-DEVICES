"""Embebe portal/portal.css en src/portal_head.h antes de compilar.

Asi el firmware y el preview del navegador (portal/portal-preview.html)
comparten el mismo CSS como fuente unica de verdad.
"""
import os

Import("env")  # noqa: F821  (inyectado por PlatformIO/SCons)

PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821
CSS_PATH = os.path.join(PROJECT_DIR, "portal", "portal.css")
OUT_PATH = os.path.join(PROJECT_DIR, "src", "portal_head.h")


def _c_string(text):
    """Convierte CSS a un literal de string C++ valido."""
    text = text.replace("\\", "\\\\")      # backslashes primero
    text = text.replace('"', '\\"')          # comillas dobles
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = text.replace("\n", "\\n")         # saltos de linea escapados
    return text


def generate_portal_head(*_args, **_kwargs):
    with open(CSS_PATH, "r", encoding="utf-8") as f:
        css = f.read()

    content = (
        "// ARCHIVO GENERADO AUTOMATICAMENTE por scripts/embed_portal.py\n"
        "// Fuente: portal/portal.css  -  NO EDITAR A MANO.\n"
        "#pragma once\n\n"
        'const char PLANNTIX_CUSTOM_HEAD[] = "<style>' + _c_string(css) + '</style>";\n'
    )

    changed = True
    if os.path.exists(OUT_PATH):
        with open(OUT_PATH, "r", encoding="utf-8") as f:
            changed = f.read() != content

    if changed:
        with open(OUT_PATH, "w", encoding="utf-8") as f:
            f.write(content)
        print("[embed_portal] src/portal_head.h actualizado desde portal/portal.css")


generate_portal_head()
env.AddPreAction("buildprog", generate_portal_head)  # noqa: F821