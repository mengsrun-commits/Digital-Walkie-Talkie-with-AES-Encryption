from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QLineEdit, QPushButton, QMessageBox,
    QGroupBox, QGraphicsOpacityEffect, QSizePolicy, QToolButton,
    QComboBox
)
from PyQt6.QtCore import Qt, QThread, pyqtSignal, QPropertyAnimation, QTimer, QPoint
from PyQt6.QtGui import QFont
import time
import json
import base64
import serial
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import hashes
from serial_services import find_esp32_ports, wait_for_serial_ports


class SerialConnectionWorker(QThread):
    connected = pyqtSignal(dict)

    def run(self):
        serial_connections = wait_for_serial_ports(115200, timeout=1)
        time.sleep(2)

        print(f"ESP32 connected on: {', '.join(serial_connections.keys())}.")
        for connection in serial_connections.values():
            try:
                time.sleep(2)
                connection.reset_input_buffer()
                connection.write(b"ready\n")
                connection.flush()
                print(f"Sent READY to {connection.port}")
            except serial.SerialException as e:
                print(f"Error on {connection.port}: {e}")

        self.connected.emit(serial_connections)

class DownwardComboBox(QComboBox):
    def showPopup(self):
        super().showPopup()
        popup = self.view().window()
        popup.move(self.mapToGlobal(QPoint(0, self.height())))
        popup.resize(self.width(), popup.height())
        if self.count() > 0:
            self.view().setCurrentIndex(self.model().index(0, 0))

class LoginApp(QWidget):
    login_success = pyqtSignal()
    back_to_start = pyqtSignal()
    device_disconnected = pyqtSignal(str)
    FORM_MIN_WIDTH = 560
    FORM_MAX_WIDTH = 900
    FORM_WIDTH_RATIO = 0.7
    INPUT_MIN_WIDTH = 520
    INPUT_HEIGHT_SCALE = 2
    AUTH_GROUP_HEIGHT_SCALE = 1.1
    BUTTON_WIDTH = 120
    TOGGLE_BUTTON_WIDTH = 72
    WAITING_MESSAGE = "Waiting for Walkie-talkies to connect..."
    DISCONNECT_MESSAGE = "Device Disconnected!"
    pulsing_duration_ms = 2000

    def __init__(self):
        super().__init__()
        self.serial_connections = {}
        self._device_ports = []
        self._updating_devices = False
        self.latest_packets = {}
        self.latest_packet = None
        self.setMinimumWidth(400)
        self._services_started = False
        self.setup_ui()

    def start_connection(self):
        self._services_started = True

        # Ensure UI/Service components are initialized
        if not hasattr(self, 'serial_read_timer'): self._setup_serial_reader()
        if not hasattr(self, 'device_poll_timer'): self._setup_device_polling()
        if not hasattr(self, 'connecting_dots_timer'): self._setup_connecting_animation()

        # Always start/restart timers when entering the page
        if not self.serial_read_timer.isActive(): self.serial_read_timer.start()
        if not self.device_poll_timer.isActive(): self.device_poll_timer.start()
        
        if hasattr(self, 'waiting_pulse') and self.waiting_pulse.state() != QPropertyAnimation.State.Running:
            self.waiting_pulse.start()

        # Check if we already have active connections
        active_ports = [p for p, c in self.serial_connections.items() if c.is_open]
        if active_ports:
            # Re-trigger handshake for each existing port to ensure fresh data
            for port in active_ports:
                print(f"Re-triggering READY handshake for {port}...")
                self._send_ready(port)
            
            # Update UI immediately to avoid "stuck" feeling
            self._refresh_device_list()
            return

        # No active ports, start search
        self.start_serial_connection_worker()

    def stop_connection(self):
        self._services_started = False
        
        if hasattr(self, 'serial_worker') and self.serial_worker.isRunning():
            self.serial_worker.terminate()
            self.serial_worker.wait()
            
        if hasattr(self, 'serial_read_timer'):
            self.serial_read_timer.stop()
            
        if hasattr(self, 'device_poll_timer'):
            self.device_poll_timer.stop()
            
        if hasattr(self, 'connecting_dots_timer'):
            self.connecting_dots_timer.stop()
            
        if hasattr(self, 'waiting_pulse'):
            self.waiting_pulse.stop()

    def _setup_serial_reader(self):
        self.serial_read_timer = QTimer(self)
        self.serial_read_timer.setInterval(100)  # fast polling
        self.serial_read_timer.timeout.connect(self._read_serial_data)
        # self.serial_read_timer.start()  # Moved to start_connection

    def _selected_port(self) -> str:
        data = self.device_combo.currentData()
        if data:
            return str(data)
        return self.device_combo.currentText().strip()

    def _format_device_label(self, port: str) -> str:
        msg = self.latest_packets.get(port)
        device_name = None
        if isinstance(msg, dict):
            device_name = msg.get("device")
        if device_name:
            return f"{port} ({device_name})"
        return port
    
    def _send_ready(self, port):
        connection = self.serial_connections.get(port)
        if connection and connection.is_open:
            try:
                connection.write(b"ready\n")
                print(f"Sent READY to {port}")
            except Exception as e:
                print(f"Failed to send READY to {port}: {e}")
                # Connection is likely dead (unplugged/replugged)
                try:
                    connection.close()
                except:
                    pass
                if port in self.serial_connections:
                    del self.serial_connections[port]
                if port in self.latest_packets:
                    del self.latest_packets[port]
                
                # Re-trigger connection search to find the new handle
                QTimer.singleShot(100, self.start_connection)

    def _read_serial_data(self):
        selected_port = self._selected_port()

        for port, connection in self.serial_connections.items():
            try:
                while connection.in_waiting:
                    line = connection.readline().decode(errors="ignore").strip()
                    if not line:
                        continue

                    # DEBUG: Print raw lines to help diagnose handshake issues
                    print(f"[{port}] RX: {line}")

                    try:
                        msg = json.loads(line)
                        self.latest_packets[port] = msg
                        if port == selected_port:
                            self.latest_packet = msg
                            self._show_login_ready()
                            self._show_login_ready()

                        if port in self._device_ports:
                            self._rebuild_device_combo(selected_port, self._device_ports)

                        print("\nEncrypted packet received ✔")
                        print("Ciphertext:", base64.b64decode(msg["ciphertext"]))

                    except json.JSONDecodeError:
                        continue

            except serial.SerialException:
                continue
        
    def setup_ui(self):
        main_layout = QVBoxLayout()
        main_layout.setSpacing(18)
        main_layout.setContentsMargins(30, 30, 30, 30)

        header_layout = QHBoxLayout()
        self.back_to_start_btn = QPushButton("Back to start")
        self.back_to_start_btn.setFixedWidth(120)
        self.back_to_start_btn.setFixedHeight(32)
        self.back_to_start_btn.clicked.connect(self.back_to_start.emit)
        header_layout.addWidget(self.back_to_start_btn, alignment=Qt.AlignmentFlag.AlignLeft)
        main_layout.addLayout(header_layout)

        self.waiting_label = QLabel(self.WAITING_MESSAGE)
        self.waiting_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.waiting_label.setWordWrap(True)
        self.waiting_label.setFont(QFont("Arial", 18, QFont.Weight.Bold))
        self.waiting_opacity = QGraphicsOpacityEffect(self.waiting_label)
        self.waiting_label.setGraphicsEffect(self.waiting_opacity)
        self.waiting_pulse = QPropertyAnimation(self.waiting_opacity, b"opacity", self)
        self.waiting_pulse.setDuration(self.pulsing_duration_ms)
        self.waiting_pulse.setKeyValueAt(0.0, 0.35) # (0 -> 1, opacity)
        self.waiting_pulse.setKeyValueAt(0.5, 1.0)
        self.waiting_pulse.setKeyValueAt(1.0, 0.35)
        self.waiting_pulse.setLoopCount(-1)
        self.waiting_pulse.start()
        main_layout.addWidget(self.waiting_label)
        self.disconnect_label = QLabel(self.DISCONNECT_MESSAGE)
        self.disconnect_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.disconnect_label.setWordWrap(True)
        self.disconnect_label.setFont(QFont("Arial", 18, QFont.Weight.Bold))
        self.disconnect_label.setStyleSheet("color: red;")

        self.disconnect_label.hide()  # hidden by default
        main_layout.addWidget(self.disconnect_label)

        self.connecting_label = QLabel("Establishing connection")
        self.connecting_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.connecting_label.setWordWrap(True)
        self.connecting_label.setFont(QFont("Arial", 16, QFont.Weight.Bold))
        self.connecting_label.setStyleSheet("color: #1a4b8c;")
        self.connecting_label.hide()
        main_layout.addWidget(self.connecting_label)

        self._setup_connecting_animation()

        self._build_login_form()
        self._update_form_width()
        self._setup_device_polling()

        # Place the compact form in the center vertically
        main_layout.addStretch()
        main_layout.addWidget(self.form_widget, alignment=Qt.AlignmentFlag.AlignHCenter)
        main_layout.addStretch()
        self._set_login_visible(False)
        self.setLayout(main_layout)

    def _build_login_form(self):
        # Form container: keep widgets grouped, compact and centered
        self.form_widget = QWidget()
        self.form_layout = QVBoxLayout()
        self.form_layout.setSpacing(14)
        self.form_layout.setContentsMargins(0, 0, 0, 0)
        self.form_widget.setLayout(self.form_layout)
        self.form_widget.setMinimumWidth(self.FORM_MIN_WIDTH)
        self.form_widget.setMaximumWidth(self.FORM_MAX_WIDTH)
        self.form_widget.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)

        self.title_label = QLabel("Login to your device")
        self.title_label.setFont(QFont("Arial", 25, QFont.Weight.Bold))
        self.title_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.form_layout.addWidget(self.title_label, alignment=Qt.AlignmentFlag.AlignHCenter)

        self.subtitle_label = QLabel("Encrypted Comms System")
        self.subtitle_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.form_layout.addWidget(self.subtitle_label, alignment=Qt.AlignmentFlag.AlignHCenter)

        # ── Group: Login Inputs ──────────────────────────────
        self.auth_group = QGroupBox("Authentication")
        g_layout = QVBoxLayout()
        g_layout.setSpacing(10)

        g_layout.addWidget(QLabel("Encryption Key / Password:"))
        self.password_input = QLineEdit()
        self.password_input.setPlaceholderText("Enter secret key...")
        self.password_input.setStyleSheet("padding-left: 7px;")
        self.password_input.setEchoMode(QLineEdit.EchoMode.Password)
        self.password_input.setMinimumWidth(self.INPUT_MIN_WIDTH)
        self.password_input.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        input_height = int(self.password_input.sizeHint().height() * self.INPUT_HEIGHT_SCALE)
        self.password_input.setFixedHeight(input_height)
        self.password_input.returnPressed.connect(self.on_login)

        self.password_toggle = QToolButton()
        self.password_toggle.setCheckable(True)
        self.password_toggle.setText("Show")
        self.password_toggle.setToolTip("Show or hide password")
        self.password_toggle.setFixedHeight(input_height)
        self.password_toggle.setFixedWidth(self.TOGGLE_BUTTON_WIDTH)
        self.password_toggle.toggled.connect(self._on_toggle_password_visibility)

        input_row = QHBoxLayout()
        input_row.setSpacing(8)
        input_row.addWidget(self.password_input)
        input_row.addWidget(self.password_toggle)
        g_layout.addLayout(input_row)

        self.auth_group.setLayout(g_layout)
        self.auth_group.setMinimumWidth(self.FORM_MIN_WIDTH)
        self.auth_group.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
        auth_group_height = int(self.auth_group.sizeHint().height() * self.AUTH_GROUP_HEIGHT_SCALE)
        self.auth_group.setMinimumHeight(auth_group_height)
        self.form_layout.addWidget(self.auth_group, alignment=Qt.AlignmentFlag.AlignHCenter)

        # ── Buttons ───────────────────────────────────────────
        self.buttons_layout = QHBoxLayout()
        self.buttons_layout.setSpacing(12)

        self.login_btn = QPushButton("Login")
        self.login_btn.setFixedHeight(36)
        self.login_btn.setFixedWidth(self.BUTTON_WIDTH)
        self.login_btn.clicked.connect(self.on_login)

        self.clear_btn = QPushButton("Clear")
        self.clear_btn.setFixedHeight(36)
        self.clear_btn.setFixedWidth(self.BUTTON_WIDTH)
        self.clear_btn.clicked.connect(self.on_clear)

        self.buttons_layout.addWidget(self.login_btn)
        self.buttons_layout.addWidget(self.clear_btn)
        self.form_layout.addLayout(self.buttons_layout)
        self.form_layout.setAlignment(self.buttons_layout, Qt.AlignmentFlag.AlignHCenter)

        # ── Device Dropdown ─────────────────────────────────
        self.device_label = QLabel("Connected Device:")
        self.device_combo = DownwardComboBox()
        self.device_combo.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.device_combo.currentIndexChanged.connect(self._pin_selected_port)
        device_row = QHBoxLayout()
        device_row.setSpacing(8)
        device_row.addWidget(self.device_label)
        device_row.addWidget(self.device_combo, stretch=1)
        self.form_layout.addLayout(device_row)

        # ── Output Label ──────────────────────────────────────
        self.status_label = QLabel("")
        self.status_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.status_label.setWordWrap(False)
        self.status_label.setStyleSheet("color: blue; font-weight: bold;")
        self.status_label.setMinimumHeight(self.status_label.sizeHint().height())
        self.status_label.setMinimumWidth(self.FORM_MIN_WIDTH)
        self.form_layout.addWidget(self.status_label, alignment=Qt.AlignmentFlag.AlignHCenter)

    def _set_login_visible(self, visible: bool):
        for widget in (
            self.title_label,
            self.subtitle_label,
            self.auth_group,
            self.login_btn,
            self.clear_btn,
            self.device_label,
            self.device_combo,
            self.status_label,
        ):
            widget.setVisible(visible)

    def _on_toggle_password_visibility(self, checked: bool):
        if checked:
            self.password_input.setEchoMode(QLineEdit.EchoMode.Normal)
            self.password_toggle.setText("Hide")
        else:
            self.password_input.setEchoMode(QLineEdit.EchoMode.Password)
            self.password_toggle.setText("Show")

    def _setup_connecting_animation(self):
        self._connecting_dots = 0
        self.connecting_dots_timer = QTimer(self)
        self.connecting_dots_timer.setInterval(400)
        self.connecting_dots_timer.timeout.connect(self._animate_connecting_label)

    def _animate_connecting_label(self):
        self._connecting_dots = (self._connecting_dots + 1) % 4
        dots = "." * self._connecting_dots
        self.connecting_label.setText(f"Establishing connection{dots}")

    def _setup_device_polling(self):
        self.device_poll_timer = QTimer(self)
        self.device_poll_timer.setInterval(1000)
        self.device_poll_timer.timeout.connect(self._refresh_device_list)
        # self.device_poll_timer.start()  # Moved to start_connection
        self._refresh_device_list()

    def _refresh_device_list(self):
        ports = find_esp32_ports()
        had_devices = bool(self._device_ports)  # <-- previous state

        # PROACTIVE CLEANUP: Remove any connections that are no longer physically present
        active_tracked_ports = list(self.serial_connections.keys())
        for port in active_tracked_ports:
            if port not in ports:
                print(f"Device {port} was unplugged. Cleaning up...")
                try:
                    self.serial_connections[port].close()
                except:
                    pass
                del self.serial_connections[port]
                if port in self.latest_packets:
                    del self.latest_packets[port]
                self.device_disconnected.emit(port)
            else:
                # Send heartbeat to keep connection alive on ESP32
                try:
                    conn = self.serial_connections[port]
                    if conn.is_open:
                        conn.write(b"ping\n")
                except Exception as e:
                    pass

        if ports != self._device_ports:
            self._device_ports = list(ports)
            selected = self._selected_port()
            self._rebuild_device_combo(selected, self._device_ports)

        if ports:
            self.waiting_label.hide()
            selected = self._selected_port()
            if selected and self.latest_packets.get(selected):
                self._show_login_ready()
            else:
                self._show_connecting_screen()
        
        else:
            if had_devices:  # <-- use previous state
                self._show_disconnect_message()
            else:
                self._show_waiting_screen()

    def _rebuild_device_combo(self, selected: str, ports: list[str]):
        self._updating_devices = True
        self.device_combo.blockSignals(True)
        self.device_combo.clear()
        if selected in ports:
            ordered_ports = [selected] + [port for port in ports if port != selected]
        else:
            ordered_ports = list(ports)

        for port in ordered_ports:
            self.device_combo.addItem(self._format_device_label(port), port)

        if ordered_ports:
            self.device_combo.setCurrentIndex(0)
        self.device_combo.blockSignals(False)
        self._updating_devices = False

    def _pin_selected_port(self):
        if self._updating_devices:
            return
        selected = self._selected_port()
        if selected:
            self._rebuild_device_combo(selected, self._device_ports)
            self.latest_packet = self.latest_packets.get(selected)
            if self.latest_packet:
                self._show_login_ready()
            else:
                self._show_connecting_screen()

    def _show_waiting_screen(self):
        self.disconnect_label.hide()
        self.connecting_label.hide()
        self.connecting_dots_timer.stop()
        self.waiting_label.setText(self.WAITING_MESSAGE)
        self.waiting_label.show()
        self._set_login_visible(False)

    def _show_disconnect_message(self):
        self.waiting_label.hide()
        self.connecting_label.hide()
        self.connecting_dots_timer.stop()
        self.disconnect_label.show()
        self._set_login_visible(False)
        QTimer.singleShot(2500, self._show_waiting_screen)

    def _show_connecting_screen(self):
        self.waiting_label.hide()
        self.disconnect_label.hide()
        self.connecting_label.show()
        if not self.connecting_dots_timer.isActive():
            self._connecting_dots = 0
            self.connecting_dots_timer.start()
        self._set_login_visible(False)

    def _show_login_ready(self):
        self.waiting_label.hide()
        self.disconnect_label.hide()
        self.connecting_label.hide()
        self.connecting_dots_timer.stop()
        self._set_login_visible(True)

    def _update_form_width(self):
        target_width = int(self.width() * self.FORM_WIDTH_RATIO)
        target_width = max(self.FORM_MIN_WIDTH, min(target_width, self.FORM_MAX_WIDTH))
        self.form_widget.setFixedWidth(target_width)
        self.auth_group.setFixedWidth(target_width)
        self.status_label.setFixedWidth(target_width)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._update_form_width()

    def on_serial_connected(self, serial_connections):
        self.serial_connections = serial_connections
        self.waiting_label.hide()
        selected = self._selected_port()
        if not selected and self._device_ports:
            selected = self._device_ports[0]
            self._rebuild_device_combo(selected, self._device_ports)

        if selected:
            self.latest_packet = self.latest_packets.get(selected)        
            if self.latest_packet:
                self._show_login_ready()
            else:
                self._show_connecting_screen()
        
    def start_serial_connection_worker(self):
        self.serial_worker = SerialConnectionWorker()
        self.serial_worker.connected.connect(self.on_serial_connected)
        self.serial_worker.start()


    def on_login(self):
        password = self.password_input.text().strip()

        if not password:
            QMessageBox.warning(self, "Missing Input", "Please enter the Encryption Key.")
            return

        if not self.latest_packet:
            self.status_label.setStyleSheet("color: red; font-weight: bold;")
            self.status_label.setText("❌ No encrypted message received yet.")
            return

        msg = self.latest_packet

        salt = base64.b64decode(msg["salt"])
        nonce = base64.b64decode(msg["nonce"])
        ciphertext = base64.b64decode(msg["ciphertext"])
        tag = base64.b64decode(msg["tag"])

        kdf = PBKDF2HMAC(
            algorithm=hashes.SHA256(),
            length=32,
            salt=salt,
            iterations=10000
        )

        key = kdf.derive(password.encode())

        try:
            cipher = Cipher(
                algorithms.AES(key),
                modes.GCM(nonce, tag)
            )

            decryptor = cipher.decryptor()
            plaintext = decryptor.update(ciphertext) + decryptor.finalize()

            print("\n✅ Correct password!")
            print("Decrypted message:", plaintext.decode())

            # Notify ESP32 of successful login
            selected_port = self._selected_port()
            conn = self.serial_connections.get(selected_port)
            if conn and conn.is_open:
                try:
                    conn.write(b"login_ok\n")
                    print(f"Sent LOGIN_OK to {selected_port}")
                except:
                    pass

            self.status_label.setStyleSheet("color: green; font-weight: bold;")
            self.status_label.setText("✅ Welcome! Logging in...")
            QTimer.singleShot(500, self.login_success.emit)

        except Exception:
            print("❌ Wrong password")
            self.status_label.setStyleSheet("color: red; font-weight: bold;")
            self.status_label.setText("❌ Access Denied: Invalid credentials.")
            self.password_input.clear()

        QTimer.singleShot(2000, self.status_label.clear)

    def on_clear(self):
        self.password_input.clear()
        self.status_label.setText("")

    def clear_credentials(self):
        self.password_input.clear()
        self.status_label.setText("")

    def _get_selected_connection(self):
        port = self._selected_port()
        if not port:
            return None, "❌ No device selected."

        existing = self.serial_connections.get(port)
        if existing and existing.is_open:
            return existing, None

        try:
            connection = serial.Serial(port, 115200, timeout=1)
        except serial.SerialException as exc:
            return None, f"❌ Failed to open {port}: {exc}"

        self.serial_connections[port] = connection
        return connection, None
    def _send_ready(self, port: str):
        if not port:
            return

        connection = self.serial_connections.get(port)
        if not connection or not connection.is_open:
            return

        try:
            connection.reset_input_buffer() 
            connection.write(b"ready\n")
            connection.flush()
            print(f"Sent READY to {port}")
        except serial.SerialException as e:
            print(f"Failed to send READY to {port}: {e}")
    