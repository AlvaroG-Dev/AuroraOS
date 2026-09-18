import sys, base64
if len(sys.argv) >= 3:
    path = sys.argv[1]
    b64data = sys.argv[2]
    with open(path, wb) as f:
        f.write(base64.b64decode(b64data))
    print(Written:, path)
