"""在指定 CBID 的写作页里跑一段 JS（自带注入 site.js）。

用法：python tools/wpage.py <cbid> "<expr>"
      python tools/wpage.py <cbid> --file x.js
"""
import json
import sys
import urllib.request
import websocket

urllib.request.install_opener(urllib.request.build_opener(urllib.request.ProxyHandler({})))
PORT = 9222


def targets():
    return json.load(urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json/list", timeout=8))


def main():
    cbid = sys.argv[1]
    args = sys.argv[2:]
    if not args:
        print("need expr")
        return 2
    src = open(args[1], encoding="utf-8").read() if args[0] == "--file" else args[0]

    t = next((t for t in targets()
              if "#/chapter/" in t.get("url", "") and cbid in t.get("url", "")), None)
    if not t:
        print("no write page for", cbid)
        return 3

    ws = websocket.create_connection(t["webSocketDebuggerUrl"], timeout=90)

    def ev(expr):
        ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate",
                            "params": {"expression": expr, "returnByValue": True,
                                       "awaitPromise": True, "userGesture": True}}))
        while True:
            m = json.loads(ws.recv())
            if m.get("id") == 1:
                return m

    ev("try{delete window.__yw}catch(e){window.__yw=null}")
    site = open("js/site.js", encoding="utf-8").read()
    ev(site)
    m = ev(src)
    ws.close()
    res = m.get("result", {})
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
