from PyQt6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QPushButton, QLabel
from PyQt6.QtCore import Qt, pyqtSignal, QTimer
from PyQt6.QtGui import QFont


class ProgramPage(QWidget):
	back_to_login = pyqtSignal()
	back_to_start = pyqtSignal()

	def __init__(self):
		super().__init__()
		self.setWindowTitle("Program")
		self._is_preview = False
		self._build_ui()

	def _build_ui(self):
		self.main_layout = QVBoxLayout()
		self.main_layout.setContentsMargins(24, 24, 24, 24)

		self.header_layout = QHBoxLayout()

		self.back_button = QPushButton("Back to Login")
		self.back_button.setFixedHeight(32)
		self.back_button.clicked.connect(self._on_back_clicked)
		self.header_layout.addWidget(self.back_button, alignment=Qt.AlignmentFlag.AlignLeft)

		self.main_layout.addLayout(self.header_layout)

		self.content_widget = QWidget()
		self.content_layout = QVBoxLayout(self.content_widget)
		self.content_layout.setContentsMargins(0, 0, 0, 0)

		self.title = QLabel("Program Page")
		self.title.setFont(QFont("Arial", 18, QFont.Weight.Bold))
		self.title.setAlignment(Qt.AlignmentFlag.AlignCenter)
		self.content_layout.addWidget(self.title, alignment=Qt.AlignmentFlag.AlignHCenter)

		self.content_layout.addStretch()
		self.main_layout.addWidget(self.content_widget)

		# Disconnect Overlay (hidden by default)
		self.disconnect_label = QLabel("Device Disconnected!")
		self.disconnect_label.setStyleSheet("color: #ff4d4d; font-weight: bold; font-size: 20px;")
		self.disconnect_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
		self.disconnect_label.hide()
		self.main_layout.addWidget(self.disconnect_label, alignment=Qt.AlignmentFlag.AlignCenter)

		self.setLayout(self.main_layout)

	def set_back_mode(self, is_preview: bool):
		self._is_preview = is_preview
		if is_preview:
			self.back_button.setText("Back to start")
		else:
			self.back_button.setText("Back to Login")

	def show_disconnect_and_exit(self):
		"""Show disconnect message and redirect to start page after a delay."""
		self.content_widget.hide()
		self.back_button.hide()
		self.disconnect_label.show()
		# Automatically go back to start after 2.5 seconds
		QTimer.singleShot(2500, self.back_to_start.emit)

	def _on_back_clicked(self):
		if self._is_preview:
			self.back_to_start.emit()
		else:
			self.back_to_login.emit()
		
	def _on_back_to_start_clicked(self):
		self.back_to_start.emit()
