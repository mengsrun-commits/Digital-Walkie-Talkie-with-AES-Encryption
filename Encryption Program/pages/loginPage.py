from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QLineEdit, QPushButton, QMessageBox,
    QGroupBox, QGraphicsOpacityEffect, QSizePolicy, QToolButton,
    QComboBox, QDialog, QDialogButtonBox
)
from PyQt6.QtCore import Qt, pyqtSignal, QPropertyAnimation, QTimer, QPoint
from PyQt6.QtGui import QFont
import time
import json
import base64
import serial
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import hashes
from serial_services import find_esp32_ports


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
        self._ready_sent_at = {}
        self.last_device_password = ""
        self.setMinimumWidth(400)
        self._services_started = False
        self._password_required_redirecting = False
        self._password_prompt_in_progress = False
        self._password_session_configured = False
        self.setup_ui()

    def start_connection(self):
        """Starts serial polling and handshake only when entering Login page."""
        self._services_started = True
        self._password_required_redirecting = False
        self._password_session_configured = False
        self.latest_packets.clear()
        self.latest_packet = None
        self._ready_sent_at.clear()
        self.password_input.clear()
        self.status_label.setText("")

        if not hasattr(self, 'serial_read_timer'):
            self._setup_serial_reader()
        if not hasattr(self, 'device_poll_timer'):
            self._setup_device_polling()
        if not hasattr(self, 'connecting_dots_timer'):
            self._setup_connecting_animation()

        if not self.serial_read_timer.isActive():
            self.serial_read_timer.start()
        if not self.device_poll_timer.isActive():
            self.device_poll_timer.start()

        if hasattr(self, 'waiting_pulse') and self.waiting_pulse.state() != QPropertyAnimation.State.Running:
            self.waiting_pulse.start()

        # Run immediate poll
        self._refresh_device_list()

    def stop_connection(self, close_connections=True):
        """Stops all serial communication and timers."""
        self._services_started = False

        if hasattr(self, 'serial_read_timer'):
            self.serial_read_timer.stop()

        if hasattr(self, 'device_poll_timer'):
            self.device_poll_timer.stop()

        if hasattr(self, 'connecting_dots_timer'):
            self.connecting_dots_timer.stop()

        if hasattr(self, 'waiting_pulse'):
            self.waiting_pulse.stop()

        if close_connections:
            for connection in list(self.serial_connections.values()):
                try:
                    if connection and connection.is_open:
                        connection.close()
                except Exception:
                    pass
            self.serial_connections.clear()
            self.latest_packets.clear()
            self.latest_packet = None
            self._ready_sent_at.clear()
            self._device_ports.clear()

    def _setup_serial_reader(self):
        self.serial_read_timer = QTimer(self)
        self.serial_read_timer.setInterval(50)  # fast non-blocking polling
        self.serial_read_timer.timeout.connect(self._read_serial_data)

    def _setup_device_polling(self):
        self.device_poll_timer = QTimer(self)
        self.device_poll_timer.setInterval(800)
        self.device_poll_timer.timeout.connect(self._refresh_device_list)

    def _setup_connecting_animation(self):
        self._connecting_dots = 0
        self.connecting_dots_timer = QTimer(self)
        self.connecting_dots_timer.setInterval(400)
        self.connecting_dots_timer.timeout.connect(self._animate_connecting_label)

    def _animate_connecting_label(self):
        self._connecting_dots = (self._connecting_dots + 1) % 4
        dots = "." * self._connecting_dots
        self.connecting_label.setText(f"Establishing connection{dots}")

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

    def _device_password_enabled(self) -> bool:
        if self._password_session_configured:
            return True
        if isinstance(self.latest_packet, dict):
            return bool(self.latest_packet.get("password_enabled", True))
        return True

    def _refresh_device_list(self):
        if not self._services_started or self._password_required_redirecting:
            return

        ports = find_esp32_ports()
        had_devices = bool(self._device_ports)

        # Cleanup any disconnected ports
        for port in list(self.serial_connections.keys()):
            if port not in ports:
                print(f"Device {port} was unplugged. Cleaning up...")
                try:
                    self.serial_connections[port].close()
                except Exception:
                    pass
                self.serial_connections.pop(port, None)
                self.latest_packets.pop(port, None)
                self._ready_sent_at.pop(port, None)
                self.device_disconnected.emit(port)

        if ports != self._device_ports:
            self._device_ports = list(ports)
            selected = self._selected_port()
            self._rebuild_device_combo(selected, self._device_ports)

        if ports:
            # Open serial port for any newly detected device
            for port in ports:
                conn = self.serial_connections.get(port)
                if not conn or not conn.is_open:
                    try:
                        conn = serial.Serial(port, 115200, timeout=0.1)
                        self.serial_connections[port] = conn
                        self._ready_sent_at.pop(port, None)
                        print(f"Opened ESP32 serial port {port}.")
                    except serial.SerialException as exc:
                        print(f"Could not open {port}: {exc}")

            # Send handshake or heartbeat
            now = time.monotonic()
            for port, conn in list(self.serial_connections.items()):
                if not conn or not conn.is_open:
                    continue
                if port not in self.latest_packets:
                    # Not yet handshaked: send READY periodically (every 1s)
                    if now - self._ready_sent_at.get(port, 0) >= 1.0:
                        try:
                            conn.write(b"ready\n")
                            conn.flush()
                            self._ready_sent_at[port] = now
                            print(f"Sent READY to {port}")
                        except Exception as exc:
                            print(f"Failed to send READY to {port}: {exc}")
                else:
                    # Handshake already complete: send periodic heartbeat ping
                    try:
                        conn.write(b"ping\n")
                        conn.flush()
                    except Exception:
                        pass

            self.waiting_label.hide()
            selected = self._selected_port()
            if selected and self.latest_packets.get(selected):
                self._show_login_ready()
            else:
                self._show_connecting_screen()
        else:
            if had_devices:
                self._show_disconnect_message()
            else:
                self._show_waiting_screen()

    def _read_serial_data(self):
        if not self._services_started:
            return

        selected_port = self._selected_port()

        for port, connection in list(self.serial_connections.items()):
            if not connection or not connection.is_open:
                continue
            try:
                while connection.in_waiting:
                    line = connection.readline().decode(errors="ignore").strip()
                    if not line:
                        continue

                    print(f"[{port}] RX: {line}")

                    try:
                        msg = json.loads(line)
                        if isinstance(msg, dict) and "ciphertext" in msg:
                            self.latest_packets[port] = msg
                            if port == selected_port or not self.latest_packet:
                                self.latest_packet = msg
                                self._show_login_ready()

                            if port in self._device_ports:
                                self._rebuild_device_combo(selected_port, self._device_ports)

                            print(f"\nEncrypted handshake packet received from {port} ✔")
                            print("Ciphertext:", base64.b64decode(msg["ciphertext"]))
                    except json.JSONDecodeError:
                        continue
            except (serial.SerialException, OSError) as exc:
                print(f"Serial read error on {port}: {exc}")

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
        self.waiting_pulse.setKeyValueAt(0.0, 0.35)
        self.waiting_pulse.setKeyValueAt(0.5, 1.0)
        self.waiting_pulse.setKeyValueAt(1.0, 0.35)
        self.waiting_pulse.setLoopCount(-1)
        main_layout.addWidget(self.waiting_label)

        self.disconnect_label = QLabel(self.DISCONNECT_MESSAGE)
        self.disconnect_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.disconnect_label.setWordWrap(True)
        self.disconnect_label.setFont(QFont("Arial", 18, QFont.Weight.Bold))
        self.disconnect_label.setStyleSheet("color: red;")
        self.disconnect_label.hide()
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
        self._setup_serial_reader()

        # Place the compact form in the center vertically
        main_layout.addStretch()
        main_layout.addWidget(self.form_widget, alignment=Qt.AlignmentFlag.AlignHCenter)
        main_layout.addStretch()
        self._set_login_visible(False)
        self.setLayout(main_layout)

    def _build_login_form(self):
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
        if self._password_required_redirecting:
            return

        if getattr(self, '_is_prompting_password', False):
            return

        if self._password_prompt_in_progress:
            return

        self.waiting_label.hide()
        self.disconnect_label.hide()
        self.connecting_label.hide()
        self.connecting_dots_timer.stop()

        password_configured = self._device_password_enabled()
        if not password_configured:
            self._is_prompting_password = True
            self._password_prompt_in_progress = True
            try:
                new_password = self._prompt_create_password()
            finally:
                self._is_prompting_password = False
                self._password_prompt_in_progress = False

            if not new_password:
                self._show_password_required_dialog()
                return

            self._password_session_configured = True
            if isinstance(self.latest_packet, dict):
                self.latest_packet["password_enabled"] = True
            for pkt in self.latest_packets.values():
                if isinstance(pkt, dict):
                    pkt["password_enabled"] = True

            selected_port = self._selected_port()
            conn = self.serial_connections.get(selected_port)
            if conn and conn.is_open:
                try:
                    encoded = base64.b64encode(new_password.encode()).decode()
                    conn.write(f"device_password_b64:{encoded}\n".encode())
                    conn.flush()
                    time.sleep(0.1)
                    conn.write(b"ready\n")
                    conn.flush()
                    print(f"Saved device password and requested fresh handshake on {selected_port}")
                except Exception as exc:
                    print(f"Failed to save new device password: {exc}")

            self.latest_packet = None
            self.password_input.clear()
            self.status_label.setStyleSheet("color: blue; font-weight: bold;")
            self.status_label.setText("Password saved. Please log in again.")
            self._set_login_visible(True)
            return

        self._set_login_visible(True)

    def _show_password_required_dialog(self):
        self._password_required_redirecting = True
        self._set_login_visible(False)
        self.stop_connection(close_connections=True)
        QMessageBox.information(
            self,
            "Password Required",
            "A device password is required. You cannot continue to the program page without setting a password."
        )
        self._password_required_redirecting = False
        self.back_to_start.emit()

    def _update_form_width(self):
        target_width = int(self.width() * self.FORM_WIDTH_RATIO)
        target_width = max(self.FORM_MIN_WIDTH, min(target_width, self.FORM_MAX_WIDTH))
        self.form_widget.setFixedWidth(target_width)
        self.auth_group.setFixedWidth(target_width)
        self.status_label.setFixedWidth(target_width)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._update_form_width()

    def on_login(self):
        password = self.password_input.text().strip()
        password_configured = self._device_password_enabled()
        pending_device_password = ""

        if password_configured:
            if not password:
                QMessageBox.warning(self, "Missing Input", "Please enter your password")
                return
            effective_password = password
        else:
            pending_device_password = password
            effective_password = ""

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

        key = kdf.derive(effective_password.encode())

        try:
            cipher = Cipher(
                algorithms.AES(key),
                modes.GCM(nonce, tag)
            )

            decryptor = cipher.decryptor()
            plaintext = decryptor.update(ciphertext) + decryptor.finalize()

            print("\n✅ Correct password!")
            print("Decrypted message:", plaintext.decode())

            if pending_device_password or password:
                self._password_session_configured = True
                if isinstance(self.latest_packet, dict):
                    self.latest_packet["password_enabled"] = True
                for pkt in self.latest_packets.values():
                    if isinstance(pkt, dict):
                        pkt["password_enabled"] = True

            self.last_device_password = pending_device_password or password

            # Notify ESP32 of successful login
            selected_port = self._selected_port()
            conn = self.serial_connections.get(selected_port)
            if conn and conn.is_open:
                try:
                    if pending_device_password:
                        encoded_device_password = base64.b64encode(pending_device_password.encode()).decode()
                        conn.write(f"device_password_b64:{encoded_device_password}\n".encode())
                        conn.flush()
                        time.sleep(0.1)
                    conn.write(b"login_ok\n")
                    conn.flush()
                    print(f"Sent LOGIN_OK to {selected_port}")
                except Exception as exc:
                    print(f"Failed to send LOGIN_OK: {exc}")

            self.status_label.setStyleSheet("color: green; font-weight: bold;")
            self.status_label.setText("✅ Welcome! Logging in...")
            QTimer.singleShot(500, self.login_success.emit)

        except Exception:
            print("❌ Wrong password")
            self.status_label.setStyleSheet("color: red; font-weight: bold;")
            self.status_label.setText("❌ Access Denied: Invalid credentials.")
            self.password_input.clear()

        QTimer.singleShot(2000, self.status_label.clear)

    def _prompt_create_password(self) -> str:
        self._set_login_visible(False)

        dialog = QDialog(self)
        dialog.setWindowTitle("Create Device Password")

        layout = QVBoxLayout(dialog)
        layout.setSpacing(12)

        info_label = QLabel("No device password is set. Create one to continue.")
        info_label.setWordWrap(True)
        layout.addWidget(info_label)

        password_input = QLineEdit()
        password_input.setPlaceholderText("New password")
        password_input.setEchoMode(QLineEdit.EchoMode.Password)

        confirm_input = QLineEdit()
        confirm_input.setPlaceholderText("Confirm password")
        confirm_input.setEchoMode(QLineEdit.EchoMode.Password)

        layout.addWidget(password_input)
        layout.addWidget(confirm_input)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        layout.addWidget(buttons)

        result = ""
        while True:
            if dialog.exec() != QDialog.DialogCode.Accepted:
                break

            new_password = password_input.text().strip()
            confirm_password = confirm_input.text().strip()
            if not new_password:
                QMessageBox.warning(self, "Missing Input", "Please enter a new password.")
                continue
            if new_password != confirm_password:
                QMessageBox.warning(self, "Mismatch", "Passwords do not match.")
                continue

            result = new_password
            break

        self._set_login_visible(True)
        return result

    def on_clear(self):
        self.password_input.clear()
        self.status_label.setText("")

    def clear_credentials(self):
        self.password_input.clear()
        self.status_label.setText("")
