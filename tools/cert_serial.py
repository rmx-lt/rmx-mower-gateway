"""Serial side of tools/provision_cert.ps1 (wiki Mower-Gateway-Tools).

  python -I tools/cert_serial.py csr   COM6 out.csr          reset the board, save its CSR
  python -I tools/cert_serial.py send  COM6 chain.pem        send the signed chain, wait for the answer
  python -I tools/cert_serial.py clear COM6                  delete the stored certificate (password login again)

The board keeps its key and every other NVS key; only the "chain" key is written or erased
(main/devcert.c). Nothing secret passes through here: the CSR and the chain are public.
"""
import sys
import time

import serial

BEGIN = "=====RMX-CSR-BEGIN====="
END = "=====RMX-CSR-END====="


def open_port(port, reset):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    if reset:
        s.rts = True
        time.sleep(0.1)
        s.rts = False
    return s


def read_until(s, marker, seconds):
    seen = ""
    t = time.time()
    while time.time() - t < seconds:
        seen += s.read(4096).decode("utf-8", "replace")
        if marker in seen:
            return seen
    return None


def send_lines(s, lines):
    for line in lines:
        s.write((line + "\n").encode())
        s.flush()
        time.sleep(0.03)


def answer(s, seconds=10):
    seen = read_until(s, "RMX-CERT-", seconds)
    if seen is None:
        return None
    time.sleep(0.5)
    seen += s.read(4096).decode("utf-8", "replace")
    for line in seen.splitlines():
        if line.startswith("RMX-CERT-"):
            return line.strip()
    return None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, port = sys.argv[1], sys.argv[2]
    if cmd == "csr":
        s = open_port(port, True)
        seen = read_until(s, END, 30)
        if seen is None:
            sys.exit("no CSR within 30 s: the board may already hold a certificate (send RMX-CSR by 'csr-now')")
        out = seen.split(BEGIN, 1)[1].split(END, 1)[0].strip()
        open(sys.argv[3], "w", newline="\n").write(out + "\n")
        print("CSR saved to", sys.argv[3])
    elif cmd == "csr-now":   # board already running, with or without certificate
        s = open_port(port, False)
        time.sleep(0.5)
        send_lines(s, ["RMX-CSR"])
        seen = read_until(s, END, 10)
        if seen is None:
            sys.exit("no CSR answer")
        out = seen.split(BEGIN, 1)[1].split(END, 1)[0].strip()
        open(sys.argv[3], "w", newline="\n").write(out + "\n")
        print("CSR saved to", sys.argv[3])
    elif cmd == "send":
        pem = [l.rstrip() for l in open(sys.argv[3]) if l.strip()]
        s = open_port(port, True)
        time.sleep(4)   # let the board boot; the command task starts after about 2 s
        s.reset_input_buffer()
        send_lines(s, ["RMX-CERT-BEGIN"] + pem + ["RMX-CERT-END"])
        res = answer(s)
        print(res or "no answer from the board")
        sys.exit(0 if res and res.startswith("RMX-CERT-OK") else 1)
    elif cmd == "clear":
        s = open_port(port, True)
        time.sleep(4)
        s.reset_input_buffer()
        send_lines(s, ["RMX-CERT-CLEAR"])
        res = answer(s)
        print(res or "no answer from the board")
        sys.exit(0 if res and res.startswith("RMX-CERT-OK") else 1)
    else:
        sys.exit(__doc__)


main()
