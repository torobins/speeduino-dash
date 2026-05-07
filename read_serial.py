import serial, time, sys

s = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
time.sleep(0.5)
s.setDTR(False)
time.sleep(0.1)
s.setDTR(True)

deadline = time.time() + 40
while time.time() < deadline:
    line = s.readline()
    if line:
        txt = line.decode('utf-8', errors='replace').strip()
        print(txt)
        sys.stdout.flush()
        if 'IP:' in txt or 'Server started' in txt:
            break

s.close()
