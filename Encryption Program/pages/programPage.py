from PyQt6.QtWidgets import (
	QWidget, QVBoxLayout, QHBoxLayout, QPushButton, QLabel, QListWidget,
	QListWidgetItem, QCheckBox, QLineEdit, QGroupBox, QFormLayout, QSizePolicy,
	QMessageBox, QInputDialog, QDialog, QDialogButtonBox
)
from PyQt6.QtCore import Qt, pyqtSignal, QTimer, QProcess
from PyQt6.QtGui import QFont
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
		self._flash_process = None
		self._pending_radio_password = ""
		self._pending_device_name = ""
		self._pending_device_password = ""
		self._pending_device_password_enabled = True
		self._settings_dirty = False
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

	def reload_channel_config(self):
		self._min_channel, self._max_channel, self._encrypted_channels = self._read_ino_channel_config()
		self._saved_encrypted_channels = set(self._encrypted_channels)
		self._populate_channel_list()

	def _read_ino_channel_config(self):
		if not self.INO_PATH.exists():
			self.channel_status_label.setText(f"Could not find sketch: {self.INO_PATH}")
			return self.DEFAULT_MIN_CHANNEL, self.DEFAULT_MAX_CHANNEL, set()

		source = self.INO_PATH.read_text(encoding="utf-8")
		min_channel = self._read_uint8_constant(source, "MIN_CHANNEL_VALUE", self.DEFAULT_MIN_CHANNEL)
		max_channel = self._read_uint8_constant(source, "MAX_CHANNEL_VALUE", self.DEFAULT_MAX_CHANNEL)
		encrypted_channels = self._read_encrypted_channels(source, min_channel, max_channel)
		return min_channel, max_channel, encrypted_channels

	def _read_uint8_constant(self, source: str, name: str, fallback: int):
		match = re.search(rf"const\s+uint8_t\s+{name}\s*=\s*(\d+)\s*;", source)
		if not match:
			return fallback
		return int(match.group(1))

	def _read_encrypted_channels(self, source: str, min_channel: int, max_channel: int):
		match = re.search(r"const\s+uint8_t\s+ENCRYPTED_CHANNELS\[\]\s*=\s*\{([^}]*)\}\s*;", source, re.DOTALL)
		if not match:
			return set()
		return {
			int(value)
			for value in re.findall(r"\d+", match.group(1))
			if min_channel <= int(value) <= max_channel
		}

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
		self.channel_status_label.clear()
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
		self._set_channel_status("Unsaved channel changes. Click Set Configuration to upload them.")

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

	def _write_encrypted_channels_to_ino(self, channels=None):
		if not self.INO_PATH.exists():
			self._set_channel_status(f"Could not save. Sketch not found: {self.INO_PATH}", is_error=True)
			return False

		source = self.INO_PATH.read_text(encoding="utf-8")
		if channels is None:
			channels = self._encrypted_channels

		channels = sorted(
			channel
			for channel in channels
			if self._min_channel <= channel <= self._max_channel
		)
		channel_list = ", ".join(str(channel) for channel in channels)
		replacement = f"const uint8_t ENCRYPTED_CHANNELS[] = {{{channel_list}}};"
		updated_source, replacements = re.subn(
			r"const\s+uint8_t\s+ENCRYPTED_CHANNELS\[\]\s*=\s*\{[^}]*\}\s*;",
			replacement,
			source,
			count=1,
			flags=re.DOTALL,
		)

		if replacements != 1:
			self._set_channel_status("Could not save. ENCRYPTED_CHANNELS[] was not found in the sketch.", is_error=True)
			return False

		self.INO_PATH.write_text(updated_source, encoding="utf-8")
		encrypted_count = len(channels)
		plural = "" if encrypted_count == 1 else "s"
		self._set_channel_status(f"Saved {encrypted_count} encrypted channel{plural} to the sketch.")
		return True

	def _has_unsaved_changes(self):
		return self._encrypted_channels != self._saved_encrypted_channels or self._settings_dirty

	def _update_set_configuration_button(self):
		if self._flash_process and self._flash_process.state() != QProcess.ProcessState.NotRunning:
			self.set_configuration_button.setEnabled(False)
			self.set_configuration_button.setText("Uploading Configuration...")
			return

		self.set_configuration_button.setEnabled(True)
		if self._has_unsaved_changes():
			self.set_configuration_button.setText("Set Configuration")
		else:
			self.set_configuration_button.setText("Set Configuration")

	def _save_pending_changes(self):
		if self._settings_dirty:
			self._on_set_configuration_clicked()
			return False

		if not self._write_encrypted_channels_to_ino():
			return False

		self._saved_encrypted_channels = set(self._encrypted_channels)
		self._update_set_configuration_button()
		return True

	def _discard_pending_changes(self):
		self._encrypted_channels = set(self._saved_encrypted_channels)
		self._settings_dirty = False
		self._pending_device_name = self._device_name
		self._pending_device_password = ""
		self.title.setText(self._device_name)
		self._populate_channel_list()
		self.channel_status_label.clear()

	def _confirm_unsaved_changes(self):
		if not self._has_unsaved_changes():
			return True

		message_box = QMessageBox(self)
		message_box.setWindowTitle("Unsaved Changes")
		message_box.setText("You have unsaved configuration changes.")
		message_box.setInformativeText("Save changes to the sketch before leaving?")
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

		if self._flash_process and self._flash_process.state() != QProcess.ProcessState.NotRunning:
			return

		if not self._upload_port:
			self._set_channel_status("No ESP32 upload port selected. Log in with a connected device before setting configuration.", is_error=True)
			return

		radio_password, accepted = QInputDialog.getText(
			self,
			"Radio Password",
			"Set password to generate encryption key",
			QLineEdit.EchoMode.Password,
		)
		if not accepted:
			return

		radio_password = radio_password.strip()
		if not radio_password:
			self._set_channel_status("Radio password is required to generate the encryption key.", is_error=True)
			return
		self._pending_radio_password = radio_password

		if not self._write_encrypted_channels_to_ino():
			return

		self._set_channel_status("Uploading configuration to ESP32...")
		self._start_flash_process()

	def _open_settings_dialog(self):
		if self._is_preview:
			return

		dialog = QDialog(self)
		dialog.setWindowTitle("Settings")
		layout = QVBoxLayout(dialog)
		form = QFormLayout()

		device_name_input = QLineEdit()
		device_name_input.setText(self._pending_device_name or self._device_name)

		password_enabled_checkbox = QCheckBox("Enable device password")
		password_enabled_checkbox.setChecked(self._pending_device_password_enabled)

		device_password_input = QLineEdit()
		device_password_input.setPlaceholderText("Set device password")
		device_password_input.setEchoMode(QLineEdit.EchoMode.Password)
		device_password_input.setEnabled(password_enabled_checkbox.isChecked())
		password_enabled_checkbox.toggled.connect(device_password_input.setEnabled)

		form.addRow("Change device name:", device_name_input)
		form.addRow("", password_enabled_checkbox)
		form.addRow("Set device password:", device_password_input)
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

		device_password_enabled = password_enabled_checkbox.isChecked()
		device_password = device_password_input.text().strip()
		if device_password_enabled and not device_password:
			self._set_channel_status("Device password is required when password login is enabled.", is_error=True)
			return

		self._pending_device_name = device_name
		self._pending_device_password_enabled = device_password_enabled
		self._pending_device_password = device_password
		self._settings_dirty = True
		self.title.setText(device_name)
		self._set_channel_status("Unsaved settings changes. Click Set Configuration to upload them.")

	def _start_flash_process(self):
		args = [
			"compile",
			"--upload",
			"--fqbn",
			self.ARDUINO_FQBN,
		]
		if self._upload_port:
			args.extend(["--port", self._upload_port])
		args.append(str(self.SKETCH_DIR))

		self._flash_process = QProcess(self)
		self._flash_process.setProgram("arduino-cli")
		self._flash_process.setArguments(args)
		self._flash_process.readyReadStandardOutput.connect(self._on_flash_output)
		self._flash_process.readyReadStandardError.connect(self._on_flash_output)
		self._flash_process.finished.connect(self._on_flash_finished)
		self._flash_process.errorOccurred.connect(self._on_flash_error)
		self._flash_process.start()
		self._update_set_configuration_button()

	def _on_flash_output(self):
		if not self._flash_process:
			return

		output = bytes(self._flash_process.readAllStandardOutput()).decode(errors="ignore").strip()
		error_output = bytes(self._flash_process.readAllStandardError()).decode(errors="ignore").strip()
		latest_line = ""
		if output:
			latest_line = output.splitlines()[-1]
		if error_output:
			latest_line = error_output.splitlines()[-1]
		if latest_line:
			self._set_channel_status(latest_line)

	def _on_flash_finished(self, exit_code: int, _exit_status):
		if exit_code == 0:
			self._saved_encrypted_channels = set(self._encrypted_channels)
			self._set_channel_status("Firmware uploaded. Storing settings...")
			QTimer.singleShot(2500, self._store_configuration_on_device)
		else:
			self._set_channel_status("Upload failed. Check Arduino CLI, board package, USB port, and ESP32 boot mode.", is_error=True)
		self._update_set_configuration_button()

	def _on_flash_error(self, _error):
		self._set_channel_status("Could not start arduino-cli. Install Arduino CLI or add it to PATH.", is_error=True)
		self._update_set_configuration_button()

	def _store_configuration_on_device(self):
		if not self._pending_radio_password:
			self._set_channel_status("Configuration uploaded to ESP32.")
			return

		try:
			with serial.Serial(self._upload_port, 115200, timeout=1) as connection:
				time.sleep(2)
				if self._settings_dirty:
					encoded_name = base64.b64encode(self._pending_device_name.encode()).decode()
					connection.write(f"device_name_b64:{encoded_name}\n".encode())
					connection.flush()
					time.sleep(0.1)

					connection.write(f"device_password_enabled:{1 if self._pending_device_password_enabled else 0}\n".encode())
					connection.flush()
					time.sleep(0.1)

					if self._pending_device_password:
						encoded_device_password = base64.b64encode(self._pending_device_password.encode()).decode()
						connection.write(f"device_password_b64:{encoded_device_password}\n".encode())
						connection.flush()
						time.sleep(0.1)

				encoded_radio_password = base64.b64encode(self._pending_radio_password.encode()).decode()
				connection.write(f"radio_password_b64:{encoded_radio_password}\n".encode())
				connection.flush()
				time.sleep(0.3)
		except serial.SerialException as exc:
			self._set_channel_status(f"Firmware uploaded, but Preferences could not be updated: {exc}", is_error=True)
			return

		self._pending_radio_password = ""
		self._pending_device_password = ""
		self._settings_dirty = False
		self._set_channel_status("Configuration uploaded and settings stored in ESP32 Preferences.")

	def _set_channel_status(self, message: str, is_error: bool = False):
		color = "#b00020" if is_error else "#1f6f43"
		self.channel_status_label.setStyleSheet(f"color: {color}; font-weight: bold;")
		self.channel_status_label.setText(message)

	def set_back_mode(self, is_preview: bool):
		self._is_preview = is_preview
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
			self.channel_status_label.clear()
		else:
			self._sync_encryption_checkbox()
			self._update_set_configuration_button()

	def show_disconnect_and_exit(self):
		"""Show disconnect message and redirect to start page after a delay."""
		self.content_widget.hide()
		self.header_widget.hide()
		self.set_configuration_button.hide()
		self.disconnect_label.show()
		# Automatically go back to start after 2.5 seconds
		QTimer.singleShot(2500, self.back_to_start.emit)

	def _on_back_clicked(self):
		if not self._confirm_unsaved_changes():
			return

		if self._is_preview:
			self.back_to_start.emit()
		else:
			self.back_to_login.emit()
		
	def _on_back_to_start_clicked(self):
		self.back_to_start.emit()
