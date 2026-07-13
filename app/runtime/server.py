# app/runtime/server.py
import json
import os
import signal
import socketserver
import sys

from runtime.router import dispatch

class RequestHandler(socketserver.StreamRequestHandler):
    def handle(self):
        while True:
            line = self.rfile.readline()
            if not line:
                break

            try:
                packet = json.loads(line)
                response = dispatch(packet)

                if "id" in packet:
                    response["id"] = packet["id"]
            except Exception as e:
                response = {
                    "ok": False,
                    "error": str(e)
                }

            self.wfile.write(
                (json.dumps(response) + "\n").encode()
            )
            self.wfile.flush()

class ThreadedUnixServer(
    socketserver.ThreadingMixIn,
    socketserver.UnixStreamServer
):
    daemon_threads = True
    allow_reuse_address = True


def run(sock="/tmp/evolution.sock"):
    if os.path.exists(sock):
        os.unlink(sock)

    server = ThreadedUnixServer(
        sock,
        RequestHandler
    )

    os.chmod(sock, 0o666)

    print(f"[IPC] {sock}")

    def stop(*_):
        server.shutdown()
        server.server_close()

        if os.path.exists(sock):
            os.unlink(sock)

        sys.exit(0)

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    server.serve_forever()

if __name__ == "__main__":
    run()
