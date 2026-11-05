"""对客户端调试端口跑一段 JS（排障 / 摸页面结构用）。

用法：
    python tools/cdp_eval.py "表达式"            # 在第一个匹配的页面里跑
    python tools/cdp_eval.py --page write "..."  # 只在 write-app 里跑
    python tools/cdp_eval.py --file x.js
结果按 JSON 打印（字符串原样、对象 indent）。
"""
import json
import sys
import urllib.request

import websocket

PORT = 9222


def targets():
    with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json/list", timeout=8) as r:
        return json.load(r)


def pick(page):
    for t in targets():
        if t.get("type") != "page":
            continue
        if page == "write" and "write" not in t.get("title", "") + t.get("url", ""):
            continue
        return t
    return None


def main():
    args = sys.argv[1:]
    page = "write"
    if args[:1] == ["--page"]:
        page = args[1]
        args = args[2:]
    if args[:1] == ["--file"]:
        src = open(args[1], encoding="utf-8").read()
    elif args:
        src = args[0]
    else:
        print("need JS expression")
        return 2

    t = pick(page)
    if not t:
        print("no target")
        return 3

    ws = websocket.create_connection(t["webSocketDebuggerUrl"], timeout=15)
    ws.send(json.dumps({
        "id": 1,
        "method": "Runtime.evaluate",
        "params": {"expression": src, "returnByValue": True, "awaitPromise": True},
    }))
    while True:
        msg = json.loads(ws.recv())
        if msg.get("id") == 1:
            break
    ws.close()

    res = msg.get("result", {})
    if "exceptionDetails" in res:
        print("EXCEPTION:", json.dumps(res["exceptionDetails"], ensure_ascii=False)[:1500])
        return 4
    v = res.get("result", {}).get("value")
    if isinstance(v, str):
        try:
            v = json.loads(v)
        except Exception:
            pass
    print(json.dumps(v, ensure_ascii=False, indent=2) if not isinstance(v, str) else v)
    return 0


if __name__ == "__main__":
    sys.exit(main())
