#!/usr/bin/env python3

import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PAGE_SIZE = 4096

class Handler(BaseHTTPRequestHandler):

    def do_GET(self):
        prefix = "/page/"
        if not self.path.startswith(prefix):
            self.send_error(404)
            return
        try:
            page_number = int(self.path[len(prefix):])
        except ValueError:
            self.send_error(400)
            return

        if page_number < 0:
            self.send_error(400)
            return

        data = str(page_number)
        data = "SOME DATA - page:" + str(page_number) + "\0"
        data = data.encode('ascii')
        remaining = PAGE_SIZE - len(data)
        pattern = b'AB'
        repeats = (remaining + len(pattern) - 1) // len(pattern)
        data = data + (pattern * repeats)[:remaining]

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        print(fmt % args)


server = ThreadingHTTPServer(
    ("0.0.0.0", 8080),
    Handler
)
print("Listening on 0.0.0.0:8080")
server.serve_forever()