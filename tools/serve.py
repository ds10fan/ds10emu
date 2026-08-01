#!/usr/bin/env python3
"""serve - host the browser build and collect what it reports back.

    python3 tools/serve.py [port]

Serves over HTTPS when web/cert.pem and web/key.pem are there, because an
AudioWorklet needs a secure context and a phone on the LAN is not localhost.
The certificate is self-signed, so the browser will want that accepted once.

`python3 -m http.server` is enough to load the page but tells you nothing
when it breaks, and on a phone there is no console to look at. This serves
web/build the same way and adds one endpoint:

    POST /log    body is a line; it is appended to out/weblog.txt and echoed

The page posts its startup breadcrumbs and anything it throws, so a black
screen on a device with no console still says why.
"""
import datetime
import gzip
import http.server
import io
import os
import ssl
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                    "web", "build")
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "out",
                   "weblog.txt")
CERT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web",
                    "cert.pem")
KEY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web",
                   "key.pem")


# Compressed copies, made once and kept. The asset bundle is 4 MB of tile
# data and compresses to a third of that, which matters a great deal to a
# phone on the far side of a LAN - a stalled download is the difference
# between the page starting and not.
_gz = {}


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=os.path.abspath(ROOT), **kw)

    def send_head(self):
        path = self.translate_path(self.path)
        if (os.path.isfile(path) and os.path.getsize(path) > 65536 and
                "gzip" in self.headers.get("Accept-Encoding", "")):
            key = (path, os.path.getmtime(path))
            if key not in _gz:
                _gz.clear()
                with open(path, "rb") as f:
                    _gz[key] = gzip.compress(f.read(), 6)
            body = _gz[key]
            self.send_response(200)
            self.send_header("Content-Type", self.guess_type(path))
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            return io.BytesIO(body)
        return super().send_head()

    def do_POST(self):
        if self.path != "/log":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n).decode("utf-8", "replace")
        stamp = datetime.datetime.now().strftime("%H:%M:%S")
        line = "%s  %s\n" % (stamp, body.replace("\n", "\n            "))
        with open(LOG, "a") as f:
            f.write(line)
        sys.stdout.write(line)
        sys.stdout.flush()
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

    def end_headers(self):
        # No caching, so a reload always gets the build that is on disk.
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        pass          # the access log drowns out the interesting part


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8537
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, "a") as f:
        f.write("---- serving on :%d ----\n" % port)
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
    scheme = "http"
    if os.path.exists(CERT) and os.path.exists(KEY):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(CERT, KEY)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
        scheme = "https"
    print("serving %s on :%d" % (scheme, port))
    sys.stdout.flush()
    srv.serve_forever()


if __name__ == "__main__":
    main()
