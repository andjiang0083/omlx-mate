"""把 Mac 端 agent / 安装脚本嵌入固件 (构建前自动生成 src/assets.h)."""
Import("env")
from pathlib import Path

root = Path(env.subst("$PROJECT_DIR"))
out = root / "src" / "assets.h"
items = [
    ("AGENT_PY", root / "agent" / "omlx_mate_agent.py", "OMLXAGENT"),
    ("SETUP_SH", root / "agent" / "setup.sh", "OMLXSETUP"),
    ("UNINSTALL_SH", root / "agent" / "uninstall.sh", "OMLXUNIN"),
]
chunks = ["// 自动生成: tools/embed_assets.py (勿手改)\n#pragma once\n#include <Arduino.h>\n\n"]
for name, path, delim in items:
    data = path.read_text(encoding="utf-8")
    if f'){delim}"' in data:
        raise SystemExit(f"{path} contains raw-string terminator {delim}")
    chunks.append(f'static const char {name}[] PROGMEM = R"{delim}({data}){delim}";\n\n')
text = "".join(chunks)
if not out.exists() or out.read_text(encoding="utf-8") != text:
    out.write_text(text, encoding="utf-8")
    print(f"[embed] wrote {out} ({len(text)} bytes)")
env.Depends("$BUILD_DIR/src/web.cpp.o", [str(root / "agent" / n) for n in
            ("omlx_mate_agent.py", "setup.sh", "uninstall.sh")])
