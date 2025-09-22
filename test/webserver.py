from http.server import BaseHTTPRequestHandler, HTTPServer

class SimpleHTTPRequestHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        print(f"> Incomming POST request: {self.path}")
        self.print_headers()
        self.print_body()
        
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"Ok")

    def do_GET(self):
        print(f"> Incomming GET request: {self.path}")
        self.print_headers()

        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"Ok")

    def print_headers(self):
        print("Headers:")
        for key, value in self.headers.items():
            print(f"\t{key}: {value}")
    
    def print_body(self):
        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length)
        print("\nBody:")
        print(body.decode('utf-8', errors='replace'))

if __name__ == "__main__":
    server_address = ('', 9000)
    httpd = HTTPServer(server_address, SimpleHTTPRequestHandler)
    print("Serving on port 9000...")
    httpd.serve_forever()