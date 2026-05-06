from PyQt6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QPushButton, QLabel
from PyQt6.QtCore import Qt, pyqtSignal
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
		main_layout = QVBoxLayout()
		main_layout.setContentsMargins(24, 24, 24, 24)

		header_layout = QHBoxLayout()

		self.back_button = QPushButton("Back to Login")
		self.back_button.setFixedHeight(32)
		self.back_button.clicked.connect(self._on_back_clicked)
		header_layout.addWidget(self.back_button, alignment=Qt.AlignmentFlag.AlignLeft)

		main_layout.addLayout(header_layout)

		title = QLabel("Program Page")
		title.setFont(QFont("Arial", 18, QFont.Weight.Bold))
		title.setAlignment(Qt.AlignmentFlag.AlignCenter)
		main_layout.addWidget(title, alignment=Qt.AlignmentFlag.AlignHCenter)

		main_layout.addStretch()
		self.setLayout(main_layout)

	def set_back_mode(self, is_preview: bool):
		self._is_preview = is_preview
		if is_preview:
			self.back_button.setText("Back to start")
		else:
			self.back_button.setText("Back to Login")

	def _on_back_clicked(self):
		if self._is_preview:
			self.back_to_start.emit()
		else:
			self.back_to_login.emit()
		
	def _on_back_to_start_clicked(self):
		self.back_to_start.emit()
