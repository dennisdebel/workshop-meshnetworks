from http.server import HTTPServer, SimpleHTTPRequestHandler
import threading
import time

PORT = 8000


class Handler(SimpleHTTPRequestHandler):

    def end_headers(self):
        self.send_header(
            "Cache-Control",
            "no-cache, no-store, must-revalidate"
        )
        super().end_headers()



def run_server():

    server = HTTPServer(
        ("", PORT),
        Handler
    )

    print(
        f"Serving at http://localhost:{PORT}"
    )

    server.serve_forever()



threading.Thread(
    target=run_server,
    daemon=True
).start()


while True:
    time.sleep(10)
