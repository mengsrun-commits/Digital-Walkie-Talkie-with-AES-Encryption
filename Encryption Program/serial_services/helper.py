import serial
from serial.tools import list_ports
import time

def find_esp32_ports():
    ports = list(list_ports.comports())
    matched_ports = []

    for port in ports:
        details = " ".join(
            str(value).lower()
            for value in (port.description, port.manufacturer, port.hwid)
            if value
        )

        if any(keyword in details for keyword in ("esp32", "cp210", "ch340", "usb serial", "uart")):
            matched_ports.append(port.device)

    if not matched_ports and len(ports) == 1:
        matched_ports.append(ports[0].device)

    return matched_ports

def wait_for_serial_ports(baudrate=115200, timeout=1, poll_interval=1):
    while True:
        port_names = find_esp32_ports()

        if port_names:
            serial_connections = {}

            for port_name in port_names:
                try:
                    serial_connections[port_name] = serial.Serial(port_name, baudrate, timeout=timeout)
                except serial.SerialException:
                    continue

            if serial_connections:
                return serial_connections

        print("Waiting for ESP32 serial ports...")
        time.sleep(poll_interval)

