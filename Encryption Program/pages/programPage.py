from PyQt6.QtWidgets import (
	QWidget, QVBoxLayout, QHBoxLayout, QPushButton, QLabel, QListWidget,
	QListWidgetItem, QCheckBox, QLineEdit, QGroupBox, QFormLayout, QSizePolicy,
	QMessageBox, QInputDialog, QDialog, QDialogButtonBox, QToolButton
)
from PyQt6.QtCore import Qt, pyqtSignal, QTimer, QSize, QPointF
from PyQt6.QtGui import QFont, QIcon, QPixmap, QPainter, QPen, QColor
from pathlib import Path
import base64
import re
import serial
import time


class ProgramPage(QWidget):
	back_to_login = pyqtSignal()
	back_to_start = pyqtSignal()
	INO_PATH = Path(__file__).resolve().parents[1] / "nRF_Walkie_talkie_encryption" / "nRF_Walkie_talkie_encryption.ino"
	SKETCH_DIR = INO_PATH.parent
	ARDUINO_FQBN = "esp32:esp32:esp32"
	DEFAULT_MIN_CHANNEL = 80
	DEFAULT_MAX_CHANNEL = 125

	def __init__(self):
		super().__init__()
		self.setWindowTitle("Program")
		self._is_preview = False
		self._device_name = "Walkie-Talkie Device"
		self._min_channel = self.DEFAULT_MIN_CHANNEL
		self._max_channel = self.DEFAULT_MAX_CHANNEL
		self._encrypted_channels = set()
		self._saved_encrypted_channels = set()
		self._updating_channels = False
		self._upload_port = ""
		self._serial_connection = None
		self._pending_radio_password = ""
		self._pending_device_name = ""
		self._pending_device_password = ""
		self._settings_dirty = False
		self._unsaved_channel_notice_shown = False
		self._unsaved_settings_notice_shown = False
		self._status_clear_timer = QTimer(self)
		self._status_clear_timer.setSingleShot(True)
		self._status_clear_timer.timeout.connect(self._clear_channel_status)
		self._heartbeat_timer = QTimer(self)
		self._heartbeat_timer.setInterval(1000)
		self._heartbeat_timer.timeout.connect(self._send_heartbeat)
		self._build_ui()

	def _build_ui(self):
		self.main_layout = QVBoxLayout()
		self.main_layout.setSpacing(20)
		self.main_layout.setContentsMargins(24, 24, 24, 24)

		self.header_widget = QWidget()
		self.header_layout = QHBoxLayout(self.header_widget)
		self.header_layout.setContentsMargins(0, 0, 0, 0)
		self.header_layout.setSpacing(12)

		self.back_button = QPushButton("Back to Login")
		self.back_button.setFixedHeight(32)
		self.back_button.setFixedWidth(120)
		self.back_button.clicked.connect(self._on_back_clicked)

		self.title = QLabel(self._device_name)
		self.title.setFont(QFont("Arial", 18, QFont.Weight.Bold))
		self.title.setAlignment(Qt.AlignmentFlag.AlignCenter)
		self.title.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)

		self.settings_button = QPushButton("Settings")
		self.settings_button.setFixedHeight(32)
		self.settings_button.setFixedWidth(120)
		self.settings_button.clicked.connect(self._open_settings_dialog)

		self.header_layout.addWidget(self.back_button)
		self.header_layout.addWidget(self.title, stretch=1)
		self.header_layout.addWidget(self.settings_button)
		self.main_layout.addWidget(self.header_widget)

		self.content_widget = QWidget()
		self.content_layout = QHBoxLayout(self.content_widget)
		self.content_layout.setContentsMargins(0, 0, 0, 0)
		self.content_layout.setSpacing(18)

		self.channels_group = QGroupBox("Channels")
		self.channels_layout = QVBoxLayout(self.channels_group)
		self.channels_layout.setSpacing(10)

		self.channel_list = QListWidget()
		self.channel_list.currentItemChanged.connect(self._on_current_channel_changed)
		self.channels_layout.addWidget(self.channel_list)

		self.encryption_group = QGroupBox("Encryption")
		self.encryption_layout = QVBoxLayout(self.encryption_group)
		self.encryption_layout.setSpacing(14)

		self.enable_encryption_checkbox = QCheckBox("Enable Encryption (AES-256)")
		self.enable_encryption_checkbox.toggled.connect(self._on_enable_encryption_toggled)
		self.encryption_layout.addWidget(self.enable_encryption_checkbox)

		self.encryption_form = QFormLayout()
		self.encryption_form.setLabelAlignment(Qt.AlignmentFlag.AlignLeft)
		self.encryption_form.setFormAlignment(Qt.AlignmentFlag.AlignTop)
		self.encryption_form.setHorizontalSpacing(14)
		self.encryption_form.setVerticalSpacing(12)

		self.salt_input = QLineEdit()
		self.salt_input.setPlaceholderText("Set salt")
		self.salt_input.setMinimumHeight(34)

		self.encryption_form.addRow("Set salt:", self.salt_input)
		self.encryption_layout.addLayout(self.encryption_form)

		self.channel_status_label = QLabel("")
		self.channel_status_label.setWordWrap(True)
		self.encryption_layout.addWidget(self.channel_status_label)
		self.encryption_layout.addStretch()

		self.content_layout.addWidget(self.channels_group, stretch=5)
		self.content_layout.addWidget(self.encryption_group, stretch=7)
		self.main_layout.addWidget(self.content_widget)

		self.set_configuration_button = QPushButton("Set Configuration")
		self.set_configuration_button.setMinimumHeight(52)
		self.set_configuration_button.setFont(QFont("Arial", 14, QFont.Weight.Bold))
		self.set_configuration_button.clicked.connect(self._on_set_configuration_clicked)
		self.main_layout.addWidget(self.set_configuration_button)

		# Disconnect Overlay (hidden by default)
		self.disconnect_label = QLabel("Device Disconnected!")
		self.disconnect_label.setStyleSheet("color: #ff4d4d; font-weight: bold; font-size: 20px;")
		self.disconnect_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
		self.disconnect_label.hide()
		self.main_layout.addWidget(self.disconnect_label, alignment=Qt.AlignmentFlag.AlignCenter)

		self.setLayout(self.main_layout)

		self.setStyleSheet("""
			QGroupBox {
				font-weight: bold;
				border: 1px solid #c9ced6;
				border-radius: 8px;
				margin-top: 12px;
				padding: 14px;
			}
			QGroupBox::title {
				subcontrol-origin: margin;
				left: 12px;
				padding: 0 4px;
			}
			QListWidget, QLineEdit {
				border: 1px solid #c9ced6;
				border-radius: 6px;
				padding: 6px;
			}
			QPushButton {
				padding: 4px 10px;
			}
		""")
		self.reload_channel_config()

	def set_device_name(self, device_name: str):
		clean_name = (device_name or "").strip()
		self._device_name = clean_name or "Walkie-Talkie Device"
		if not self._pending_device_name:
			self._pending_device_name = self._device_name
		self.title.setText(self._device_name)

	def set_upload_port(self, port: str):
		self._upload_port = (port or "").strip()

	def set_serial_connection(self, connection):
		self._serial_connection = connection
		if connection and getattr(connection, "is_open", False) and not self._is_preview:
			self._heartbeat_timer.start()
		else:
			self._heartbeat_timer.stop()

	def stop_serial_activity(self):
		self._heartbeat_timer.stop()

	def set_device_password(self, device_password: str):
		clean_password = (device_password or "").strip()
		if clean_password:
			self._pending_device_password = clean_password

	def set_encrypted_channels(self, channels):
		self._encrypted_channels = {
			int(channel)
			for channel in channels or []
			if self._min_channel <= int(channel) <= self._max_channel
		}
		self._saved_encrypted_channels = set(self._encrypted_channels)
		self._populate_channel_list()

	def reload_channel_config(self):
		self._min_channel, self._max_channel = self._read_ino_channel_config()
		self._encrypted_channels = set()
		self._saved_encrypted_channels = set()
		self._populate_channel_list()

	def load_preview_data(self):
		self._min_channel = 80
		self._max_channel = 90
		self._encrypted_channels = {80, 83, 88}
		self._saved_encrypted_channels = set(self._encrypted_channels)
		self._populate_channel_list()
		self._set_channel_status("Preview mode: showing sample data.")

	def _read_ino_channel_config(self):
		if not self.INO_PATH.exists():
			self.channel_status_label.setText(f"Could not find sketch: {self.INO_PATH}")
			return self.DEFAULT_MIN_CHANNEL, self.DEFAULT_MAX_CHANNEL

		source = self.INO_PATH.read_text(encoding="utf-8")
		min_channel = self._read_uint8_constant(source, "MIN_CHANNEL_VALUE", self.DEFAULT_MIN_CHANNEL)
		max_channel = self._read_uint8_constant(source, "MAX_CHANNEL_VALUE", self.DEFAULT_MAX_CHANNEL)
		return min_channel, max_channel

	def _read_uint8_constant(self, source: str, name: str, fallback: int):
		match = re.search(rf"const\s+uint8_t\s+{name}\s*=\s*(\d+)\s*;", source)
		if not match:
			return fallback
		return int(match.group(1))

	def _populate_channel_list(self):
		self._updating_channels = True
		self.channel_list.clear()

		for channel_number in range(self._min_channel, self._max_channel + 1):
			item = QListWidgetItem()
			item.setData(Qt.ItemDataRole.UserRole, channel_number)
			self._set_channel_item_text(item, channel_number)
			self.channel_list.addItem(item)

		if self.channel_list.count():
			self.channel_list.setCurrentRow(0)

		self._updating_channels = False
		self._sync_encryption_checkbox()
		self._clear_channel_status()
		self._update_set_configuration_button()

	def _set_channel_item_text(self, item: QListWidgetItem, channel_number: int):
		suffix = " (Encrypted)" if channel_number in self._encrypted_channels else ""
		item.setText(f"Channel {channel_number}{suffix}")

	def _on_current_channel_changed(self, current, _previous):
		if self._updating_channels:
			return
		self._sync_encryption_checkbox()

	def _on_enable_encryption_toggled(self, checked: bool):
		if self._updating_channels or self._is_preview:
			return

		current = self.channel_list.currentItem()
		if not current:
			return

		self._set_channel_encryption(current, checked)

	def _set_channel_encryption(self, item: QListWidgetItem, enabled: bool):
		channel_number = item.data(Qt.ItemDataRole.UserRole)
		if channel_number is None:
			return

		channel_number = int(channel_number)
		if enabled:
			self._encrypted_channels.add(channel_number)
		else:
			self._encrypted_channels.discard(channel_number)

		self._updating_channels = True
		self._set_channel_item_text(item, channel_number)
		self._updating_channels = False
		self._update_set_configuration_button()
		if not self._unsaved_channel_notice_shown:
			self._unsaved_channel_notice_shown = True
			self._set_channel_status(
				"Unsaved channel changes. Click Save Configuration to store them.",
				timeout_ms=4000,
			)

	def _sync_encryption_checkbox(self):
		current = self.channel_list.currentItem()
		self._updating_channels = True
		if current:
			channel_number = current.data(Qt.ItemDataRole.UserRole)
			self.enable_encryption_checkbox.setEnabled(not self._is_preview)
			self.enable_encryption_checkbox.setChecked(int(channel_number) in self._encrypted_channels)
		else:
			self.enable_encryption_checkbox.setEnabled(False)
			self.enable_encryption_checkbox.setChecked(False)
		self._updating_channels = False

	def _has_unsaved_changes(self):
		return self._encrypted_channels != self._saved_encrypted_channels or self._settings_dirty

	def _update_set_configuration_button(self):
		self.set_configuration_button.setEnabled(True)
		self.set_configuration_button.setText("Save Configuration")

	def _save_pending_changes(self):
		return self._on_set_configuration_clicked()

	def _discard_pending_changes(self):
		self._encrypted_channels = set(self._saved_encrypted_channels)
		self._settings_dirty = False
		self._pending_device_name = self._device_name
		self.title.setText(self._device_name)
		self._populate_channel_list()
		self._clear_channel_status()

	def _confirm_unsaved_changes(self):
		if not self._has_unsaved_changes():
			return True

		message_box = QMessageBox(self)
		message_box.setWindowTitle("Unsaved Changes")
		message_box.setText("You have unsaved configuration changes.")
		message_box.setInformativeText("Save changes to the ESP32 before leaving?")
		save_button = message_box.addButton("Save", QMessageBox.ButtonRole.AcceptRole)
		discard_button = message_box.addButton("Discard", QMessageBox.ButtonRole.DestructiveRole)
		message_box.addButton("Cancel", QMessageBox.ButtonRole.RejectRole)
		message_box.exec()

		clicked_button = message_box.clickedButton()
		if clicked_button == save_button:
			return self._save_pending_changes()
		if clicked_button == discard_button:
			self._discard_pending_changes()
			return True
		return False

	def _on_set_configuration_clicked(self):
		if self._is_preview:
			return

		if not self._upload_port:
			self._set_channel_status("No ESP32 upload port selected. Log in with a connected device before setting configuration.", is_error=True)
			return False

		channels_changed = self._encrypted_channels != self._saved_encrypted_channels
		if channels_changed and self._encrypted_channels:
			radio_password, accepted = QInputDialog.getText(
				self,
				"Radio Password",
				"Set password to generate encryption key",
				QLineEdit.EchoMode.Password,
			)
			if not accepted:
				return False

			radio_password = radio_password.strip()
			if not radio_password:
				self._set_channel_status("Radio password is required when encrypted channels are enabled.", is_error=True)
				return False
			self._pending_radio_password = radio_password

		self._set_channel_status("Saving configuration to ESP32 Preferences...")
		return self._store_configuration_on_device()

	def _open_settings_dialog(self):
		if self._is_preview:
			return

		dialog = QDialog(self)
		dialog.setWindowTitle("Settings")
		layout = QVBoxLayout(dialog)
		form = QFormLayout()

		device_name_input = QLineEdit()
		device_name_input.setText(self._pending_device_name or self._device_name)

		device_password_input = QLineEdit()
		device_password_input.setPlaceholderText("Set device password")
		device_password_input.setText(self._pending_device_password)
		device_password_input.setEchoMode(QLineEdit.EchoMode.Password)

		password_toggle = QToolButton()
		password_toggle.setCheckable(True)
		password_toggle.setAutoRaise(True)
		password_toggle.setFixedSize(34, 34)
		password_toggle.setIconSize(QSize(20, 20))
		password_toggle.setCursor(Qt.CursorShape.PointingHandCursor)
		password_toggle.setIcon(self._make_eye_icon())
		password_toggle.setToolTip("Show or hide device password")
		password_toggle.toggled.connect(
			lambda checked: self._toggle_password_visibility(checked, device_password_input, password_toggle)
		)

		password_row = QHBoxLayout()
		password_row.setSpacing(8)
		password_row.addWidget(device_password_input)
		password_row.addWidget(password_toggle)

		form.addRow("Change device name:", device_name_input)
		form.addRow("Set device password:", password_row)
		layout.addLayout(form)

		buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel)
		layout.addWidget(buttons)
		buttons.accepted.connect(dialog.accept)
		buttons.rejected.connect(dialog.reject)

		if dialog.exec() != QDialog.DialogCode.Accepted:
			return

		device_name = device_name_input.text().strip()
		if not device_name:
			self._set_channel_status("Device name cannot be empty.", is_error=True)
			return

		device_password = device_password_input.text().strip()
		if not device_password:
			self._set_channel_status("Device password cannot be empty.", is_error=True)
			return

		self._pending_device_name = device_name
		self._pending_device_password = device_password
		self._settings_dirty = True
		self.title.setText(device_name)
		if not self._unsaved_settings_notice_shown:
			self._unsaved_settings_notice_shown = True
			self._set_channel_status(
				"Unsaved settings changes. Click Save Configuration to store them.",
				timeout_ms=4000,
			)

	def _toggle_password_visibility(self, checked: bool, input_field: QLineEdit, toggle_button: QToolButton):
		if checked:
			input_field.setEchoMode(QLineEdit.EchoMode.Normal)
			toggle_button.setIcon(self._make_eye_icon(slashed=True))
		else:
			input_field.setEchoMode(QLineEdit.EchoMode.Password)
			toggle_button.setIcon(self._make_eye_icon())

	def _make_eye_icon(self, slashed: bool = False):
		pixmap = QPixmap(22, 22)
		pixmap.fill(Qt.GlobalColor.transparent)

		painter = QPainter(pixmap)
		painter.setRenderHint(QPainter.RenderHint.Antialiasing)
		pen = QPen(QColor("#667085"), 1.7)
		pen.setCapStyle(Qt.PenCapStyle.RoundCap)
		painter.setPen(pen)
		painter.setBrush(Qt.BrushStyle.NoBrush)
		painter.drawEllipse(QPointF(11, 11), 7.4, 4.8)
		painter.setBrush(QColor("#667085"))
		painter.drawEllipse(QPointF(11, 11), 2.2, 2.2)

		if slashed:
			painter.setBrush(Qt.BrushStyle.NoBrush)
			painter.drawLine(5, 17, 17, 5)

		painter.end()
		return QIcon(pixmap)

	def _store_configuration_on_device(self):
		try:
			self._heartbeat_timer.stop()
			connection = self._serial_connection
			opened_here = False
			if not connection or not connection.is_open:
				connection = serial.Serial(self._upload_port, 115200, timeout=1)
				opened_here = True
				time.sleep(2)

			try:
				if connection.in_waiting:
					connection.reset_input_buffer()

				if self._settings_dirty:
					encoded_name = base64.b64encode(self._pending_device_name.encode()).decode()
					confirmed, replies = self._send_serial_command(connection, f"device_name_b64:{encoded_name}", "Device name stored.")
					if not confirmed:
						self._set_channel_status(self._format_serial_failure("Device name", replies), is_error=True)
						return False

					if self._pending_device_password:
						encoded_device_password = base64.b64encode(self._pending_device_password.encode()).decode()
						confirmed, replies = self._send_serial_command(connection, f"device_password_b64:{encoded_device_password}", "Device password stored.")
						if not confirmed:
							self._set_channel_status(self._format_serial_failure("Device password", replies), is_error=True)
							return False

				if self._pending_radio_password:
					encoded_radio_password = base64.b64encode(self._pending_radio_password.encode()).decode()
					confirmed, replies = self._send_serial_command(connection, f"radio_password_b64:{encoded_radio_password}", "Radio password stored.")
					if not confirmed:
						self._set_channel_status(self._format_serial_failure("Radio password", replies), is_error=True)
						return False

				channel_list = ",".join(str(channel) for channel in sorted(self._encrypted_channels))
				confirmed, replies = self._send_serial_command(connection, f"encrypted_channels:{channel_list}", "Encrypted channels stored.")
				if not confirmed:
					message = self._format_serial_failure("Encrypted channels", replies)
					self._set_channel_status(f"{message} Flash the updated firmware once, then try again.", is_error=True)
					return False
			finally:
				if opened_here:
					connection.close()
				elif self._serial_connection and self._serial_connection.is_open and not self._is_preview:
					self._heartbeat_timer.start()
		except serial.SerialException as exc:
			self._set_channel_status(f"Preferences could not be updated: {exc}", is_error=True)
			return False

		self._pending_radio_password = ""
		self._settings_dirty = False
		self._device_name = self._pending_device_name or self._device_name
		self._saved_encrypted_channels = set(self._encrypted_channels)
		self._update_set_configuration_button()
		self._set_channel_status("Configuration saved to ESP32 Preferences.", timeout_ms=4000)
		return True

	def _send_serial_command(self, connection, command: str, expected_reply: str, timeout: float = 5.0):
		replies = []
		for _attempt in range(3):
			if connection.in_waiting:
				connection.reset_input_buffer()
			connection.write(f"{command}\n".encode())
			connection.flush()
			deadline = time.monotonic() + timeout
			while time.monotonic() < deadline:
				if connection.in_waiting:
					reply = connection.readline().decode(errors="ignore").strip()
					if reply:
						replies.append(reply)
						if expected_reply in reply:
							return True, replies
				time.sleep(0.05)
		return False, replies

	def _send_heartbeat(self):
		connection = self._serial_connection
		if self._is_preview or not connection or not connection.is_open:
			self._heartbeat_timer.stop()
			return

		try:
			connection.write(b"heartbeat\n")
			connection.flush()
		except serial.SerialException:
			self._heartbeat_timer.stop()
			self._set_channel_status("Lost serial connection to ESP32.", is_error=True)

	def _format_serial_failure(self, setting_name: str, replies: list[str]):
		if replies:
			return f"{setting_name} was not confirmed by the ESP32. Last reply: {replies[-1]}"
		return f"{setting_name} was not confirmed by the ESP32. No serial reply was received."

	def _set_channel_status(self, message: str, is_error: bool = False, timeout_ms: int | None = None):
		self._status_clear_timer.stop()
		color = "#b00020" if is_error else "#1f6f43"
		self.channel_status_label.setStyleSheet(f"color: {color}; font-weight: bold;")
		self.channel_status_label.setText(message)
		if timeout_ms:
			self._status_clear_timer.start(timeout_ms)

	def _clear_channel_status(self):
		self._status_clear_timer.stop()
		self.channel_status_label.clear()

	def set_back_mode(self, is_preview: bool):
		self._is_preview = is_preview
		self._unsaved_channel_notice_shown = False
		self._unsaved_settings_notice_shown = False
		if is_preview:
			self.back_button.setText("Back to start")
		else:
			self.back_button.setText("Back to Login")
		self._apply_preview_mode()

	def _apply_preview_mode(self):
		self.enable_encryption_checkbox.setEnabled(not self._is_preview and self.channel_list.currentItem() is not None)
		self.settings_button.setEnabled(not self._is_preview)
		self.set_configuration_button.setEnabled(not self._is_preview)
		if self._is_preview:
			self.set_configuration_button.setText("Preview Mode")
			self._clear_channel_status()
		else:
			self._sync_encryption_checkbox()
			self._update_set_configuration_button()

	def show_disconnect_and_exit(self):
		"""Show disconnect message and redirect to start page after a delay."""
		self.stop_serial_activity()
		self.content_widget.hide()
		self.header_widget.hide()
		self.set_configuration_button.hide()
		self.disconnect_label.show()
		# Automatically go back to start after 2.5 seconds
		QTimer.singleShot(2500, self.back_to_start.emit)

	def _on_back_clicked(self):
		if not self._confirm_unsaved_changes():
			return

		self.stop_serial_activity()
		if self._is_preview:
			self.back_to_start.emit()
		else:
			self.back_to_login.emit()
		
	def _on_back_to_start_clicked(self):
		self.stop_serial_activity()
		self.back_to_start.emit()
