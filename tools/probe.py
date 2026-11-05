"""在「写作页」target（url 含 #/chapter/）里跑一段 JS。用于排查 store 结构变化。

用法：
    python tools/probe.py "表达式"
    python tools/probe.py --file x.js
    python tools/probe.py --list          # 只列目标
"""
import json
import sys
import urllib.request

import websocket

PORT = 9222


def targets():
    with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json/list", timeout=8) as r:
        return json.load(r)


def main():
    args = sys.argv[1:]
    if args[:1] == ["--list"]:
        for t in targets():
            print(t.get("type"), "|", t.get("title"), "|", t.get("url"))
        return 0
    if args[:1] == ["--file"]:
        src = open(args[1], encoding="utf-8").read()
    elif args:
        src = args[0]
    else:
        print("need JS")
        return 2

    cand = [t for t in targets()
            if t.get("type") == "page" and "#/chapter/" in t.get("url", "")]
    if not cand:
        cand = [t for t in targets() if t.get("type") == "page" and t.get("title") == "write-app"]
    if not cand:
        print("no write-app target")
        return 3
    t = cand[0]
    print("target:", t["title"], t["url"], file=sys.stderr)

    ws = websocket.create_connection(t["webSocketDebuggerUrl"], timeout=20)
    ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate",
                        "params": {"expression": src, "returnByValue": True,
                                   "awaitPromise": True}}))
    msg = None
    while True:
        msg = json.loads(ws.recv())
        if msg.get("id") == 1:
            break
    ws.close()

    res = msg.get("result", {})
    if "exceptionDetails" in res:
        print("EXCEPTION:", json.dumps(res["exceptionDetails"], ensure_ascii=False)[:2000])
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
